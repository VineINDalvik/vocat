// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "host_ws.h"
#include "pipeline_ws.h"
#include "vad.h"
#include "ws_session.h"
#include "mp3_player.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_system.h"

static const char *TAG = "host_ws";

#define FRAME_BYTES   640   // 20ms at 16kHz 16bit mono
#define B64_OUT_SIZE  ((FRAME_BYTES * 4 / 3) + 8)
#define JSON_BUF_SIZE (B64_OUT_SIZE + 32)

static esp_websocket_client_handle_t s_ws              = NULL;
static volatile bool                 s_feed_run        = false;
static TaskHandle_t                  s_feed_task_handle = NULL;
static host_ws_msg_cb_t              s_msg_cb          = NULL;
static void                         *s_msg_cb_ctx      = NULL;
static volatile bool                 s_got_done        = false;
static volatile bool                 s_sending         = false;
static volatile bool          s_session_rejected  = false;
static host_ws_rejected_cb_t  s_rejected_cb       = NULL;
static void                  *s_rejected_cb_ctx   = NULL;
// Shared flags readable by feed_task
extern volatile bool s_skip_cooldown;  // set by ws_session do_interrupt
extern volatile bool s_interrupted;    // cleared when end_of_speech sent (new question ready)
static esp_websocket_client_handle_t s_rejected_ws = NULL;  // stashed by feed_task for disconnect to clean up

void host_ws_set_callback(host_ws_msg_cb_t cb, void *ctx)
{
    s_msg_cb     = cb;
    s_msg_cb_ctx = ctx;
}

static void build_ws_uri(char *out, size_t len, const char *path, const char *session_id)
{
    const char *base = CONFIG_WS_MEETING_API_BASE_URL;
    const char *host = strstr(base, "://");
    host = host ? host + 3 : base;
    snprintf(out, len, "wss://%s%s/%s", host, path, session_id);
}

void host_ws_set_rejected_cb(host_ws_rejected_cb_t cb, void *ctx)
{
    s_rejected_cb     = cb;
    s_rejected_cb_ctx = ctx;
}

// After VAD triggers end_of_speech, keep sending audio for FLUSH_FRAMES
// more frames so the ASR has time to process buffered speech before the
// server closes the stream.
#define FLUSH_FRAMES 15   // 15 x 20ms = 300ms
#define COOLDOWN_FRAMES 50  // 50 x 20ms = 1000ms after TTS ends, skip VAD to avoid echo/noise re-trigger
#define MIN_AUDIO_FRAMES_FOR_EOS 40  // suppress end_of_speech if less than 800ms audio sent (noise guard)

static void feed_task(void *arg)
{
    ESP_LOGI(TAG, "[OK] feed task started");

    static uint8_t       pcm_buf[FRAME_BYTES];
    static unsigned char b64_buf[B64_OUT_SIZE];
    static char          json_buf[JSON_BUF_SIZE];
    vad_ctx_t vad = {0};
    uint32_t audio_frame_count = 0;
    int flush_remaining = 0;
    int cooldown_remaining = 0;
    bool prev_mic_muted = false;

    while (s_feed_run) {
        if (s_session_rejected) {
            s_feed_run = false;
            break;
        }
        int got = pipeline_ws_recorder_read(pcm_buf, FRAME_BYTES);
        if (got != FRAME_BYTES) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }

        // Detect g_mic_muted edges
        bool mic_just_muted   = !prev_mic_muted && g_mic_muted;   // rising edge: TTS started
        bool mic_just_unmuted = prev_mic_muted && !g_mic_muted;   // falling edge: TTS ended

        if (mic_just_unmuted && cooldown_remaining == 0) {
            cooldown_remaining = COOLDOWN_FRAMES;
            ESP_LOGI(TAG, "TTS ended, starting %dms cooldown", COOLDOWN_FRAMES * 20);
        }
        prev_mic_muted = g_mic_muted;

        if (g_mic_muted) {
            if (mic_just_muted) {
                vad_reset(&vad);
                flush_remaining = 0;
            }
            continue;
        }

        // Cooldown: skip VAD processing to avoid echo/noise re-trigger after TTS
        if (cooldown_remaining > 0) {
            cooldown_remaining--;
            continue;
        }

        vad_result_t result = vad_process_frame(&vad, (const int16_t *)pcm_buf,
                                                  FRAME_BYTES / 2);

        esp_websocket_client_handle_t ws = s_ws;
        if (!ws || !esp_websocket_client_is_connected(ws)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        bool should_send = s_sending &&
            (vad.state == VAD_STATE_SPEECH ||
             vad.state == VAD_STATE_SILENCE_AFTER_SPEECH ||
             flush_remaining > 0);

        if (should_send) {
            size_t b64_len = 0;
            mbedtls_base64_encode(b64_buf, sizeof(b64_buf), &b64_len,
                                  pcm_buf, FRAME_BYTES);
            b64_buf[b64_len] = '\0';
            int jlen = snprintf(json_buf, sizeof(json_buf),
                                "{\"type\":\"audio\",\"data\":\"%s\"}", b64_buf);
            int ret = esp_websocket_client_send_text(ws, json_buf, jlen, pdMS_TO_TICKS(200));
            if (ret < 0) {
                ESP_LOGW(TAG, "WS send audio failed (%d), heap=%lu",
                         ret, esp_get_free_heap_size());
                continue;
            }
            audio_frame_count++;
            if (audio_frame_count % 50 == 0) {
                ESP_LOGI(TAG, "sending audio frame #%lu heap=%lu",
                         (unsigned long)audio_frame_count, esp_get_free_heap_size());
            }
        }

        if (result == VAD_RESULT_END_OF_SPEECH && s_sending && flush_remaining == 0) {
            // Noise guard: suppress end_of_speech if too few audio frames sent
            if (audio_frame_count < MIN_AUDIO_FRAMES_FOR_EOS) {
                ESP_LOGI(TAG, "VAD end_of_speech suppressed (only %lu frames < %d, likely noise)",
                         (unsigned long)audio_frame_count, MIN_AUDIO_FRAMES_FOR_EOS);
                vad_reset(&vad);
                audio_frame_count = 0;
            } else {
                flush_remaining = FLUSH_FRAMES;
                ESP_LOGI(TAG, "VAD end_of_speech — starting %dms flush", FLUSH_FRAMES * 20);
            }
        }

        if (flush_remaining > 0) {
            flush_remaining--;
            if (flush_remaining == 0) {
                const char *eos = "{\"type\":\"end_of_speech\"}";
                int ret = esp_websocket_client_send_text(ws, eos, (int)strlen(eos), pdMS_TO_TICKS(1000));
                if (ret < 0) {
                    ESP_LOGW(TAG, "WS send end_of_speech failed (%d)", ret);
                }
                ESP_LOGI(TAG, "[OK] sent end_of_speech (after flush, %lu audio frames)",
                         (unsigned long)audio_frame_count);
                ESP_LOGI(TAG, "[LATENCY] end_of_speech_sent ts=%lldms",
                         (long long)(esp_timer_get_time() / 1000));
                s_sending = false;
                s_interrupted = false;  // user finished new question — accept next response audio
                vad_reset(&vad);
            }
        }

        if (s_got_done && !g_mic_muted && !mp3_player_pending()) {
            s_got_done = false;
            s_sending  = true;
            audio_frame_count = 0;
            flush_remaining = 0;
            if (s_skip_cooldown) {
                s_skip_cooldown = false;
                cooldown_remaining = 0;
                ESP_LOGI(TAG, "recv done + interrupt resume, VAD resumed (no cooldown)");
            } else {
                cooldown_remaining = COOLDOWN_FRAMES;
                ESP_LOGI(TAG, "recv done + TTS finished, resuming VAD (with %dms cooldown)",
                         COOLDOWN_FRAMES * 20);
            }
            vad_reset(&vad);
        }
    }

    // If rejected by server (403), save WS handle so disconnect can stop/destroy it.
    // feed_task must NOT call stop/destroy itself (race with WS internal task).
    esp_websocket_client_handle_t rejected_ws = NULL;
    if (s_session_rejected) {
        rejected_ws = s_ws;   // capture before nulling
        s_ws = NULL;
        s_rejected_ws = rejected_ws;  // stash for host_ws_disconnect
    }
    pipeline_ws_recorder_close();
    s_feed_task_handle = NULL;
    if (s_session_rejected && s_rejected_cb) {
        s_rejected_cb(s_rejected_cb_ctx);
        s_session_rejected = false;
    }
    vTaskDelete(NULL);
}

static uint32_t s_ws_evt_total = 0;

/* ── Fragment reassembly buffer ─────────────────────────────────────────── */
static char   *s_reasm_buf  = NULL;   // heap buffer for current fragmented message
static int     s_reasm_len  = 0;      // total expected bytes (payload_len)
static int     s_reasm_got  = 0;      // bytes accumulated so far

static void reasm_free(void)
{
    free(s_reasm_buf);
    s_reasm_buf = NULL;
    s_reasm_len = 0;
    s_reasm_got = 0;
}

/* Process a fully-received JSON text message (either direct or reassembled). */
static void process_ws_message(const char *data, int len)
{
    cJSON *root = cJSON_Parse(data);
    if (!root) {
        ESP_LOGE(TAG, "[DROP-JSON] len=%d peek=%.80s", len, data);
        return;
    }

    cJSON *type_j = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type_j)) {
        cJSON_Delete(root);
        return;
    }

    const char *type = type_j->valuestring;
    ESP_LOGI(TAG, "recv %s (len=%d evt#%"PRIu32")", type, len, s_ws_evt_total);

    if (strcmp(type, "done") == 0) {
        s_got_done = true;
    }
    if (s_msg_cb) {
        s_msg_cb(type, root, s_msg_cb_ctx);
    }
    cJSON_Delete(root);
}

static void ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    if (event_id == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
        s_ws_evt_total++;

        if (d->op_code != 1 || d->data_len <= 0) {
            if (d->op_code != 10 && d->op_code != 9) {
                ESP_LOGW(TAG, "[DROP] op=%d data_len=%d (evt#%"PRIu32")",
                         d->op_code, d->data_len, s_ws_evt_total);
            }
            return;
        }

        int payload_len = (int)d->payload_len;
        int data_len    = d->data_len;
        int offset      = (int)d->payload_offset;

        /* ── Case A: complete (non-fragmented) message ───────────── */
        if (payload_len == data_len) {
            char *tmp = malloc((size_t)data_len + 1);
            if (!tmp) {
                ESP_LOGE(TAG, "[DROP-MALLOC] len=%d heap=%"PRIu32,
                         data_len, (uint32_t)esp_get_free_heap_size());
                return;
            }
            memcpy(tmp, d->data_ptr, data_len);
            tmp[data_len] = '\0';
            process_ws_message(tmp, data_len);
            free(tmp);
            return;
        }

        /* ── Case B: fragmented message — reassemble ─────────────── */
        if (offset == 0) {
            reasm_free();
            if (payload_len > 256 * 1024) {
                ESP_LOGE(TAG, "[DROP-TOOBIG] payload=%d", payload_len);
                return;
            }
            s_reasm_buf = malloc((size_t)payload_len + 1);
            if (!s_reasm_buf) {
                ESP_LOGE(TAG, "[DROP-MALLOC-REASM] payload=%d heap=%"PRIu32,
                         payload_len, (uint32_t)esp_get_free_heap_size());
                return;
            }
            s_reasm_len = payload_len;
            s_reasm_got = 0;
        }

        if (!s_reasm_buf || offset != s_reasm_got || offset + data_len > s_reasm_len) {
            ESP_LOGE(TAG, "[DROP-REASM-SEQ] off=%d got=%d data=%d total=%d",
                     offset, s_reasm_got, data_len, s_reasm_len);
            reasm_free();
            return;
        }

        memcpy(s_reasm_buf + offset, d->data_ptr, data_len);
        s_reasm_got += data_len;

        if (s_reasm_got == s_reasm_len) {
            s_reasm_buf[s_reasm_len] = '\0';
            process_ws_message(s_reasm_buf, s_reasm_len);
            reasm_free();
        }

    } else if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "[OK] WS connected");
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "WS disconnected");
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
        if (d->error_handle.esp_ws_handshake_status_code == 403) {
            ESP_LOGE(TAG, "WS rejected 403 — session invalid, stopping reconnect");
            s_session_rejected = true;
        } else {
            ESP_LOGE(TAG, "WS error");
        }
    }
}

esp_err_t host_ws_connect(const char *session_id)
{
    char uri[256];
    build_ws_uri(uri, sizeof(uri), "/ws/host", session_id);
    ESP_LOGI(TAG, "connecting %s", uri);

    esp_websocket_client_config_t cfg = {
        .uri                         = uri,
        .buffer_size                 = 131072,  // 128KB: answer_audio JSON (MP3 base64) can exceed 16KB
        .task_stack                  = 12288,   // 12KB: TLS ops (mbedtls_ssl_read/write) need ~4KB stack
        .task_prio                   = 5,
        .skip_cert_common_name_check = true,
        .network_timeout_ms          = 30000,
        .reconnect_timeout_ms        = 10000,
    };
    s_ws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(s_ws);

    // Wait up to 15s for connection (TLS handshake can take >5s)
    for (int i = 0; i < 150; i++) {
        if (esp_websocket_client_is_connected(s_ws)) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!esp_websocket_client_is_connected(s_ws)) {
        ESP_LOGE(TAG, "[FAIL] connect timeout");
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[OK] connected wss://.../ws/host/%s", session_id);

    pipeline_ws_recorder_open();
    s_feed_run = true;
    s_sending  = true;
    s_got_done = false;
    xTaskCreatePinnedToCoreWithCaps(
        feed_task, "host_feed", 16 * 1024, NULL, 5,
        &s_feed_task_handle, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ESP_OK;
}

esp_err_t host_ws_disconnect(void)
{
    s_feed_run = false;
    // Null out s_ws so feed_task won't use a destroyed handle
    esp_websocket_client_handle_t ws = s_ws;
    s_ws = NULL;
    for (int i = 0; i < 30 && s_feed_task_handle != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    // feed_task already closes the recorder on exit, no need here

    // Clean up the WS client (either from this disconnect or stashed by feed_task on rejection)
    if (!ws) ws = s_rejected_ws;
    s_rejected_ws = NULL;

    if (ws) {
        if (esp_websocket_client_is_connected(ws)) {
            const char *stop = "{\"type\":\"stop\"}";
            esp_websocket_client_send_text(ws, stop, (int)strlen(stop), pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "[OK] sent stop");
        }
        esp_websocket_client_stop(ws);
        esp_websocket_client_destroy(ws);
    }
    ESP_LOGI(TAG, "[OK] disconnected, heap_free=%lu", esp_get_free_heap_size());
    return ESP_OK;
}

void host_ws_force_resume(void)
{
    s_got_done = true;   // triggers the recovery path in feed_task
    ESP_LOGI(TAG, "force resume: s_got_done set to true");
}

esp_err_t host_ws_send_interrupt(void)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) {
        ESP_LOGW(TAG, "send_interrupt: WS not connected");
        return ESP_ERR_INVALID_STATE;
    }
    const char *msg = "{\"type\":\"interrupt\"}";
    int ret = esp_websocket_client_send_text(s_ws, msg, (int)strlen(msg), pdMS_TO_TICKS(1000));
    if (ret < 0) {
        ESP_LOGW(TAG, "WS send interrupt failed (%d)", ret);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[OK] sent interrupt");
    return ESP_OK;
}
