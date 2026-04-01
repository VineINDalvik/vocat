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

#define QUEUE_DEPTH  8

typedef struct {
    uint8_t *data;
    size_t   len;
} mp3_chunk_t;

static QueueHandle_t     s_queue    = NULL;
static TaskHandle_t      s_task     = NULL;
static volatile bool     s_task_run = false;
static volatile bool     s_busy     = false;
static SemaphoreHandle_t s_done_sem = NULL;

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

    if (!pcm_raw || !pcm_resampled) {
        ESP_LOGE(TAG, "PCM buffer alloc failed");
        if (pcm_raw)       heap_caps_free(pcm_raw);
        if (pcm_resampled) heap_caps_free(pcm_resampled);
        vTaskDelete(NULL);
        return;
    }

    uint32_t chunk_count = 0;

    while (s_task_run) {
        mp3_chunk_t chunk;
        if (xQueueReceive(s_queue, &chunk, pdMS_TO_TICKS(100)) != pdTRUE) continue;

        chunk_count++;
        int64_t recv_ts = esp_timer_get_time() / 1000;
        ESP_LOGI(TAG, "[OK] playing chunk #%lu len=%u",
                 (unsigned long)chunk_count, (unsigned)chunk.len);
        ESP_LOGI(TAG, "[LATENCY] answer_audio_recv ts=%lldms", (long long)recv_ts);

        if (!s_busy) {
            s_busy       = true;
            g_mic_muted  = true;
            ESP_LOGI(TAG, "mic MUTED (playback started)");
        }

        // Decode all MP3 frames in this chunk
        int      offset      = 0;
        uint32_t mp3_frames  = 0;
        uint32_t pcm_total   = 0;
        bool     first_frame = true;

        while (offset < (int)chunk.len) {
            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame(
                &mp3d,
                chunk.data + offset,
                (int)chunk.len - offset,
                pcm_raw, &info);
            if (samples <= 0 || info.frame_bytes <= 0) break;
            offset += info.frame_bytes;
            mp3_frames++;

            if (first_frame) {
                int64_t play_ts = esp_timer_get_time() / 1000;
                ESP_LOGI(TAG, "[LATENCY] playback_start ts=%lldms delta=%lldms",
                         (long long)play_ts, (long long)(play_ts - recv_ts));
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

        ESP_LOGI(TAG, "chunk #%lu decoded: %lu mp3 frames -> %lu pcm frames (resampled)",
                 (unsigned long)chunk_count,
                 (unsigned long)mp3_frames,
                 (unsigned long)pcm_total);
        free(chunk.data);

        // After draining queue, unmute after 300ms
        if (uxQueueMessagesWaiting(s_queue) == 0) {
            s_busy = false;
            ESP_LOGI(TAG, "[OK] all chunks played, queue empty");
            vTaskDelay(pdMS_TO_TICKS(300));
            g_mic_muted = false;
            ESP_LOGI(TAG, "mic UNMUTED (300ms after last chunk)");
            if (s_done_sem) xSemaphoreGive(s_done_sem);
        }
    }

    heap_caps_free(pcm_raw);
    heap_caps_free(pcm_resampled);
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
        &s_task, 0,
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
    if (xQueueSend(s_queue, &chunk, 0) != pdTRUE) {
        free(chunk.data);
        ESP_LOGW(TAG, "queue full, dropping chunk len=%u", (unsigned)len);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
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
