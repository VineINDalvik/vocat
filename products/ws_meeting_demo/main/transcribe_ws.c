// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "transcribe_ws.h"
#include "pipeline_ws.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "transcribe_ws";

// Batch 3 frames (300ms at 16kHz 16bit mono) per send to reduce TLS alloc/free churn
#define FEED_BATCH_FRAMES  3
#define FEED_FRAME_BYTES   3200
#define FEED_BATCH_BYTES   (FEED_FRAME_BYTES * FEED_BATCH_FRAMES)
#define B64_OUT_SIZE       ((FEED_BATCH_BYTES * 4 / 3) + 8)
#define JSON_BUF_SIZE      (B64_OUT_SIZE + 32)

static esp_websocket_client_handle_t s_ws = NULL;
static volatile bool s_feed_run           = false;
static TaskHandle_t  s_feed_task_handle   = NULL;

// Wake word detection callback
static transcribe_wake_word_cb_t s_wake_word_cb = NULL;
static void                     *s_wake_word_cb_ctx = NULL;
static volatile bool             s_wake_word_triggered = false;  // debounce: one trigger per utterance

#define WAKE_WORD "开始演示样机"

static bool text_contains_wake_word(const char *text)
{
    if (!text) return false;
    return strstr(text, "开始演示样机") != NULL;
}

void transcribe_ws_set_wake_word_cb(transcribe_wake_word_cb_t cb, void *ctx)
{
    s_wake_word_cb     = cb;
    s_wake_word_cb_ctx = ctx;
}

static void build_ws_uri(char *out, size_t len, const char *path, const char *session_id)
{
    // Replace "https://" prefix with "wss://"
    const char *base = CONFIG_WS_MEETING_API_BASE_URL;
    const char *host = strstr(base, "://");
    host = host ? host + 3 : base;
    snprintf(out, len, "wss://%s%s/%s", host, path, session_id);
}

static void feed_task(void *arg)
{
    ESP_LOGI(TAG, "[OK] feed task started");

    static uint8_t       pcm_buf[FEED_FRAME_BYTES];
    static uint8_t       batch_buf[FEED_BATCH_BYTES];
    static unsigned char b64_buf[B64_OUT_SIZE];
    static char          json_buf[JSON_BUF_SIZE];
    uint32_t frame_count = 0;

    while (s_feed_run) {
        int batch_len = 0;
        for (int i = 0; i < FEED_BATCH_FRAMES; i++) {
            int got = pipeline_ws_recorder_read(pcm_buf, FEED_FRAME_BYTES);
            if (got != FEED_FRAME_BYTES) { vTaskDelay(pdMS_TO_TICKS(5)); break; }
            memcpy(batch_buf + batch_len, pcm_buf, FEED_FRAME_BYTES);
            batch_len += FEED_FRAME_BYTES;
        }
        if (batch_len == 0) continue;

        esp_websocket_client_handle_t ws = s_ws;
        if (!ws || !esp_websocket_client_is_connected(ws)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        size_t b64_len = 0;
        mbedtls_base64_encode(b64_buf, sizeof(b64_buf), &b64_len,
                              batch_buf, batch_len);
        b64_buf[b64_len] = '\0';

        int jlen = snprintf(json_buf, sizeof(json_buf),
                            "{\"type\":\"audio\",\"data\":\"%s\"}", b64_buf);
        esp_websocket_client_send_text(ws, json_buf, jlen, pdMS_TO_TICKS(500));

        frame_count += batch_len / FEED_FRAME_BYTES;
        if (frame_count % 300 == 0) {
            ESP_LOGI(TAG, "sent #%lu frames total", (unsigned long)frame_count);
        }
    }

    ESP_LOGI(TAG, "[OK] feed task stopped, heap_free=%lu", esp_get_free_heap_size());
    s_feed_task_handle = NULL;
    vTaskDelete(NULL);
}

static void ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "[OK] WS connected");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WS disconnected");
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WS error");
        break;
    case WEBSOCKET_EVENT_DATA: {
        esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
        if (d->op_code != 1 || d->data_len <= 0) return;
        if (d->payload_len != d->data_len) {
            // Fragmented message — skip for simplicity (wake words come in short messages)
            return;
        }
        char *tmp = malloc((size_t)d->data_len + 1);
        if (!tmp) return;
        memcpy(tmp, d->data_ptr, d->data_len);
        tmp[d->data_len] = '\0';

        cJSON *root = cJSON_Parse(tmp);
        if (root) {
            cJSON *type_j = cJSON_GetObjectItemCaseSensitive(root, "type");
            if (cJSON_IsString(type_j) && strcmp(type_j->valuestring, "transcript") == 0) {
                cJSON *text_j   = cJSON_GetObjectItemCaseSensitive(root, "text");
                cJSON *final_j  = cJSON_GetObjectItemCaseSensitive(root, "is_final");
                const char *text = (cJSON_IsString(text_j)) ? text_j->valuestring : "";
                bool is_final    = cJSON_IsTrue(final_j);

                if (is_final) {
                    ESP_LOGI(TAG, "transcript (final): \"%s\"", text);
                    // Only reset debounce if wake word NOT in this final text
                    // Prevents double-trigger: partial triggers, then final resets debounce and re-triggers
                    if (!text_contains_wake_word(text)) {
                        s_wake_word_triggered = false;
                    }
                } else {
                    ESP_LOGI(TAG, "transcript (partial): \"%s\"", text);
                }
                // Trigger wake word on partial or final (debounced: only once per utterance)
                if (text_contains_wake_word(text) && !s_wake_word_triggered && s_wake_word_cb) {
                    s_wake_word_triggered = true;
                    ESP_LOGI(TAG, "[WAKE] \"%s\" detected → triggering host mode", WAKE_WORD);
                    s_wake_word_cb(s_wake_word_cb_ctx);
                }
            }
            cJSON_Delete(root);
        }
        free(tmp);
        break;
    }
    default: break;
    }
}

esp_err_t transcribe_ws_connect(const char *session_id)
{
    char uri[256];
    build_ws_uri(uri, sizeof(uri), "/ws/transcribe", session_id);
    ESP_LOGI(TAG, "connecting %s", uri);

    esp_websocket_client_config_t cfg = {
        .uri                         = uri,
        .buffer_size                 = 16384,
        .task_stack                  = 12288,   // 12KB: TLS ops (mbedtls_ssl_read/write) need ~4KB stack
        .task_prio                   = 5,
        .skip_cert_common_name_check = true,
        .network_timeout_ms          = 30000,
        .reconnect_timeout_ms        = 3000,    // faster reconnect (was 10s)
        .ping_interval_sec           = 3,       // keep WS alive through NAT/firewall
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
    ESP_LOGI(TAG, "[OK] connected wss://.../ws/transcribe/%s", session_id);

    pipeline_ws_recorder_open();
    s_feed_run = true;
    xTaskCreatePinnedToCoreWithCaps(
        feed_task, "transcribe_feed", 12 * 1024, NULL, 5,
        &s_feed_task_handle, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ESP_OK;
}

esp_err_t transcribe_ws_send_end(void)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return ESP_ERR_INVALID_STATE;
    const char *msg = "{\"type\":\"end\"}";
    esp_websocket_client_send_text(s_ws, msg, (int)strlen(msg), pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "[OK] sent end signal");
    return ESP_OK;
}

esp_err_t transcribe_ws_disconnect(void)
{
    s_feed_run = false;
    // Null out s_ws so feed_task won't use a destroyed handle
    esp_websocket_client_handle_t ws = s_ws;
    s_ws = NULL;
    // Wait for feed task to exit (up to 3s)
    for (int i = 0; i < 30 && s_feed_task_handle != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    pipeline_ws_recorder_close();
    if (ws) {
        esp_websocket_client_stop(ws);
        esp_websocket_client_destroy(ws);
    }
    ESP_LOGI(TAG, "[OK] disconnected");
    return ESP_OK;
}
