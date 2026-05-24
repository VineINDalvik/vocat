// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

#include "mp3_player.h"
#include "pipeline_ws.h"
#include "esp_check.h"
#include "ws_session.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "mp3_player";

#define QUEUE_DEPTH  64

typedef struct {
    uint8_t *data;
    size_t   len;
} mp3_chunk_t;

static QueueHandle_t     s_queue    = NULL;
static TaskHandle_t      s_task     = NULL;
static volatile bool     s_task_run = false;
static volatile bool     s_busy     = false;
static volatile bool     s_interrupt_flag = false;
static volatile bool     s_discontinuity  = false;  // set when enqueue drops a chunk — decoder must reset
static SemaphoreHandle_t s_done_sem = NULL;

#define DECODE_BUF_SIZE (16 * 1024)

// Resample 24kHz mono → 16kHz mono (ratio 2/3 via linear interpolation)
static int resample_24k_to_16k(const int16_t *in, int in_n, int16_t *out)
{
    int out_n = (in_n * 2) / 3;
    for (int i = 0; i < out_n; i++) {
        int p    = i * 3 / 2;
        int frac = (i * 3) % 2;
        if (frac == 0 || p + 1 >= in_n) {
            out[i] = in[p];
        } else {
            out[i] = (int16_t)(((int)in[p] + in[p + 1]) / 2);
        }
    }
    return out_n;
}

static void mp3_play_task(void *arg)
{
    ESP_LOGI(TAG, "[OK] task started");

    mp3dec_t mp3d;
    mp3dec_init(&mp3d);

    int16_t *pcm_raw = heap_caps_malloc(
        MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *pcm_resampled = heap_caps_malloc(
        MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *decode_buf = heap_caps_malloc(
        DECODE_BUF_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!pcm_raw || !pcm_resampled || !decode_buf) {
        ESP_LOGE(TAG, "buffer alloc failed");
        heap_caps_free(pcm_raw);
        heap_caps_free(pcm_resampled);
        heap_caps_free(decode_buf);
        vTaskDelete(NULL);
        return;
    }

    uint32_t chunk_count = 0;
    bool     prev_had_audio = false;
    size_t   decode_buf_len = 0; // unconsumed bytes carried across chunks

    while (s_task_run) {
        /* ── Reset state after interrupt (before receiving next chunk) ── */
        if (s_interrupt_flag) {
            s_interrupt_flag = false;
            chunk_count = 0;
            prev_had_audio = false;
            decode_buf_len = 0;
            mp3dec_init(&mp3d);
            s_busy = false;
            g_mic_muted = false;
            ESP_LOGI(TAG, "state reset after interrupt — decoder fresh");
        }

        mp3_chunk_t chunk;
        if (xQueueReceive(s_queue, &chunk, pdMS_TO_TICKS(100)) != pdTRUE) continue;

        /* ── Sentinel: server sent "done" — all audio has been delivered ── */
        if (chunk.data == NULL) {
            ESP_LOGI(TAG, "[OK] done sentinel — waiting for ring buffer drain");
            for (int i = 0; i < 100; i++) {
                if (pipeline_ws_player_is_drained()) break;
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            s_busy = false;
            g_mic_muted = false;
            ESP_LOGI(TAG, "mic UNMUTED (done + drained)");
            if (s_done_sem) xSemaphoreGive(s_done_sem);
            chunk_count = 0;
            prev_had_audio = false;
            decode_buf_len = 0;
            continue;
        }

        chunk_count++;
        int64_t recv_ts = esp_timer_get_time() / 1000;

        if (!s_busy) {
            s_busy       = true;
            g_mic_muted  = true;
            ESP_LOGI(TAG, "mic MUTED (playback started)");
        }

        /* Detect sentence boundary by ID3v2 tag */
        bool is_new_sentence = (chunk.len >= 3 &&
            chunk.data[0] == 'I' && chunk.data[1] == 'D' && chunk.data[2] == '3');

        /* After dropped chunks, decoder state is stale — skip non-sentence chunks until resync */
        if (s_discontinuity) {
            if (is_new_sentence) {
                s_discontinuity = false;
                ESP_LOGI(TAG, "decoder resync after discontinuity — at sentence boundary");
                // mp3dec_init + decode_buf_len=0 handled by is_new_sentence block below
            } else {
                free(chunk.data);
                continue;
            }
        }

        if (is_new_sentence) {
            mp3dec_init(&mp3d);
            decode_buf_len = 0; // discard leftover from previous sentence

            if (prev_had_audio) {
                static const int16_t silence[1600] = {0};
                pipeline_ws_player_write_pcm((int16_t *)silence, 1600);
                pipeline_ws_player_write_pcm((int16_t *)silence, 1600);
            }
        }

        /* Skip ID3v2 header if present */
        size_t data_start = 0;
        if (is_new_sentence && chunk.len >= 10) {
            uint32_t id3_body =
                ((uint32_t)(chunk.data[6] & 0x7F) << 21) |
                ((uint32_t)(chunk.data[7] & 0x7F) << 14) |
                ((uint32_t)(chunk.data[8] & 0x7F) <<  7) |
                 (uint32_t)(chunk.data[9] & 0x7F);
            uint32_t id3_total = 10 + id3_body;
            data_start = (id3_total <= (uint32_t)chunk.len) ? id3_total : chunk.len;
        }

        /* Append chunk data to decode buffer (which may hold leftover from prev chunk) */
        size_t data_to_copy = chunk.len - data_start;
        if (decode_buf_len + data_to_copy > DECODE_BUF_SIZE) {
            ESP_LOGW(TAG, "decode buf overflow, discarding leftover");
            decode_buf_len = 0;
        }
        memcpy(decode_buf + decode_buf_len, chunk.data + data_start, data_to_copy);
        decode_buf_len += data_to_copy;
        free(chunk.data);

        /* Decode all complete MP3 frames from the combined buffer */
        uint32_t mp3_frames = 0, pcm_total = 0;
        int offset = 0;
        int64_t decode_start_ts = esp_timer_get_time() / 1000;
        bool first_frame = true;

        while (offset < (int)decode_buf_len) {
            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame(
                &mp3d,
                decode_buf + offset,
                (int)decode_buf_len - offset,
                pcm_raw, &info);
            if (info.frame_bytes <= 0) break;
            offset += info.frame_bytes;
            if (samples <= 0) continue;
            mp3_frames++;

            if (first_frame) {
                int64_t play_ts = esp_timer_get_time() / 1000;
                ESP_LOGI(TAG, "[LATENCY] chunk #%lu playback_start ts=%lldms"
                         " recv_to_play=%lldms",
                         (unsigned long)chunk_count,
                         (long long)play_ts,
                         (long long)(play_ts - recv_ts));
                first_frame = false;
            }

            int out_frames;
            if (info.hz == 24000) {
                out_frames = resample_24k_to_16k(pcm_raw, samples, pcm_resampled);
                pipeline_ws_player_write_pcm(pcm_resampled, out_frames);
            } else {
                out_frames = samples;
                pipeline_ws_player_write_pcm(pcm_raw, out_frames);
            }
            pcm_total += (uint32_t)out_frames;
        }

        /* Keep unconsumed bytes for next chunk (cross-boundary MP3 frame) */
        size_t leftover = decode_buf_len - (size_t)offset;
        if (leftover > 0 && offset > 0) {
            memmove(decode_buf, decode_buf + offset, leftover);
        }
        decode_buf_len = leftover;

        int64_t decode_end_ts = esp_timer_get_time() / 1000;
        ESP_LOGI(TAG, "[CHUNK] #%lu decoded: %lu frames %lu pcm %lldms leftover=%u",
                 (unsigned long)chunk_count,
                 (unsigned long)mp3_frames,
                 (unsigned long)pcm_total,
                 (long long)(decode_end_ts - decode_start_ts),
                 (unsigned)leftover);

        if (pcm_total > 0) prev_had_audio = true;
    }

    heap_caps_free(pcm_raw);
    heap_caps_free(pcm_resampled);
    heap_caps_free(decode_buf);
    ESP_LOGI(TAG, "[OK] task stopped, heap_free=%lu", esp_get_free_heap_size());
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t mp3_player_open(void)
{
    if (s_queue) return ESP_OK; // already open

    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(mp3_chunk_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "queue create failed");

    s_done_sem = xSemaphoreCreateBinary();
    if (!s_done_sem) { vQueueDelete(s_queue); s_queue = NULL; return ESP_ERR_NO_MEM; }

    pipeline_ws_player_open();

    s_task_run = true;
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        mp3_play_task, "mp3_play", 32 * 1024, NULL, 7,
        &s_task, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        vQueueDelete(s_queue); s_queue = NULL;
        vSemaphoreDelete(s_done_sem); s_done_sem = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t mp3_player_enqueue(const uint8_t *mp3, size_t len)
{
    if (!s_queue) return ESP_ERR_INVALID_STATE;
    mp3_chunk_t chunk;
    chunk.data = malloc(len);
    if (!chunk.data) return ESP_ERR_NO_MEM;
    memcpy(chunk.data, mp3, len);
    chunk.len = len;
    if (xQueueSend(s_queue, &chunk, pdMS_TO_TICKS(500)) != pdTRUE) {
        free(chunk.data);
        s_discontinuity = true;  // signal mp3_play_task to reset decoder on next ID3v2
        ESP_LOGW(TAG, "queue full after 500ms, dropping chunk len=%u heap=%lu",
                 (unsigned)len, esp_get_free_heap_size());
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void mp3_player_signal_done(void)
{
    if (!s_queue) return;
    mp3_chunk_t sentinel = { .data = NULL, .len = 0 };
    xQueueSend(s_queue, &sentinel, pdMS_TO_TICKS(500));
}

esp_err_t mp3_player_flush_and_wait(void)
{
    if (!s_done_sem) return ESP_OK;
    xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(30000));
    return ESP_OK;
}

esp_err_t mp3_player_close(void)
{
    s_task_run = false;
    for (int i = 0; i < 50 && s_task != NULL; i++) vTaskDelay(pdMS_TO_TICKS(100));
    pipeline_ws_player_close();
    if (s_queue) {
        mp3_chunk_t chunk;
        while (xQueueReceive(s_queue, &chunk, 0) == pdTRUE) free(chunk.data);
        vQueueDelete(s_queue); s_queue = NULL;
    }
    if (s_done_sem) { vSemaphoreDelete(s_done_sem); s_done_sem = NULL; }
    g_mic_muted = false;
    s_busy      = false;
    return ESP_OK;
}

bool mp3_player_is_busy(void) { return s_busy; }

bool mp3_player_pending(void)
{
    return s_busy || (s_queue && uxQueueMessagesWaiting(s_queue) > 0);
}

void mp3_player_stop(void)
{
    if (!s_queue) return;
    s_interrupt_flag = true;
    g_mic_muted = false;
    s_busy = false;
    // Drain queue from outside — discard all pending chunks (audio and done sentinel)
    mp3_chunk_t chunk;
    while (xQueueReceive(s_queue, &chunk, 0) == pdTRUE) {
        if (chunk.data) free(chunk.data);
    }
    ESP_LOGI(TAG, "interrupt: player stopped, mic unmuted, queue drained");
}
