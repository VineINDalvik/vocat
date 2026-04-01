// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "host_ws.h"
#include "pipeline_ws.h"
#include "vad.h"
#include "ws_session.h"
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

static void feed_task(void *arg)
{
    ESP_LOGI(TAG, "[OK] feed task started");

    static uint8_t       pcm_buf[FRAME_BYTES];
    static unsigned char b64_buf[B64_OUT_SIZE];
    static char          json_buf[JSON_BUF_SIZE];
    vad_ctx_t vad = {0};
    uint32_t audio_frame_count = 0;

    while (s_feed_run) {
        if (s_session_rejected) {
            s_feed_run = false;
            break;
        }
        int got = pipeline_ws_recorder_read(pcm_buf, FRAME_BYTES);
        if (got != FRAME_BYTES) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        if (!esp_websocket_client_is_connected(s_ws)) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

        if (g_mic_muted) {
            vad_reset(&vad);
            continue;
        }

        vad_result_t result = vad_process_frame(&vad, (const int16_t *)pcm_buf,
                                                  FRAME_BYTES / 2);

        if (s_sending && vad.state == VAD_STATE_SPEECH) {
            size_t b64_len = 0;
            mbedtls_base64_encode(b64_buf, sizeof(b64_buf), &b64_len,
                                  pcm_buf, FRAME_BYTES);
            b64_buf[b64_len] = '\0';
            int jlen = snprintf(json_buf, sizeof(json_buf),
                                "{\"type\":\"audio\",\"data\":\"%s\"}", b64_buf);
            esp_websocket_client_send_text(s_ws, json_buf, jlen, pdMS_TO_TICKS(100));
            audio_frame_count++;
            if (audio_frame_count % 50 == 0) {
                ESP_LOGI(TAG, "sending audio frame #%lu", (unsigned long)audio_frame_count);
            }
        }

        if (result == VAD_RESULT_END_OF_SPEECH && s_sending) {
            const char *eos = "{\"type\":\"end_of_speech\"}";
            esp_websocket_client_send_text(s_ws, eos, (int)strlen(eos), pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "[OK] sent end_of_speech");
            ESP_LOGI(TAG, "[LATENCY] end_of_speech_sent ts=%lldms",
                     (long long)(esp_timer_get_time() / 1000));
            s_sending = false;
        }

        if (s_got_done) {
            s_got_done = false;
            s_sending  = true;
            audio_frame_count = 0;
            vad_reset(&vad);
            ESP_LOGI(TAG, "recv done, resuming VAD");
        }
    }

    // If rejected by server (403), safely stop+destroy WS client from this task
    // (cannot call esp_websocket_client_stop from within the WS event handler).
    if (s_session_rejected && s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    pipeline_ws_recorder_close();
    s_feed_task_handle = NULL;
    if (s_session_rejected && s_rejected_cb) {
        s_rejected_cb(s_rejected_cb_ctx);
        s_session_rejected = false;
    }
    vTaskDelete(NULL);
}

static void ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    if (event_id == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
        if (d->op_code != 1 || d->data_len <= 0) return; // text frames only

        char *tmp = malloc((size_t)d->data_len + 1);
        if (!tmp) return;
        memcpy(tmp, d->data_ptr, d->data_len);
        tmp[d->data_len] = '\0';

        cJSON *root = cJSON_Parse(tmp);
        free(tmp);
        if (!root) return;

        cJSON *type_j = cJSON_GetObjectItemCaseSensitive(root, "type");
        if (cJSON_IsString(type_j)) {
            const char *type = type_j->valuestring;
            ESP_LOGI(TAG, "recv %s", type);

            if (strcmp(type, "done") == 0) {
                s_got_done = true;
            }
            if (s_msg_cb) {
                s_msg_cb(type, root, s_msg_cb_ctx);
            }
        }
        cJSON_Delete(root);

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
        .buffer_size                 = 16384,
        .task_stack                  = 8192,
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
        feed_task, "host_feed", 8 * 1024, NULL, 5,
        &s_feed_task_handle, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ESP_OK;
}

esp_err_t host_ws_disconnect(void)
{
    s_feed_run = false;
    for (int i = 0; i < 30 && s_feed_task_handle != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    pipeline_ws_recorder_close();

    if (s_ws) {
        if (esp_websocket_client_is_connected(s_ws)) {
            const char *stop = "{\"type\":\"stop\"}";
            esp_websocket_client_send_text(s_ws, stop, (int)strlen(stop), pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "[OK] sent stop");
        }
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    ESP_LOGI(TAG, "[OK] disconnected, heap_free=%lu", esp_get_free_heap_size());
    return ESP_OK;
}
