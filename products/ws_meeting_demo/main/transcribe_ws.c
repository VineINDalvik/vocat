// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "transcribe_ws.h"
#include "pipeline_ws.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "transcribe_ws";

// 100ms frame at 16kHz 16bit mono
#define FEED_FRAME_BYTES  3200
#define B64_OUT_SIZE      ((FEED_FRAME_BYTES * 4 / 3) + 8)
#define JSON_BUF_SIZE     (B64_OUT_SIZE + 32)

static esp_websocket_client_handle_t s_ws = NULL;
static volatile bool s_feed_run           = false;
static TaskHandle_t  s_feed_task_handle   = NULL;

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
    static unsigned char b64_buf[B64_OUT_SIZE];
    static char          json_buf[JSON_BUF_SIZE];
    uint32_t frame_count = 0;

    while (s_feed_run) {
        int got = pipeline_ws_recorder_read(pcm_buf, FEED_FRAME_BYTES);
        if (got != FEED_FRAME_BYTES) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        if (!esp_websocket_client_is_connected(s_ws)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        size_t b64_len = 0;
        mbedtls_base64_encode(b64_buf, sizeof(b64_buf), &b64_len,
                              pcm_buf, FEED_FRAME_BYTES);
        b64_buf[b64_len] = '\0';

        int jlen = snprintf(json_buf, sizeof(json_buf),
                            "{\"type\":\"audio\",\"data\":\"%s\"}", b64_buf);
        esp_websocket_client_send_text(s_ws, json_buf, jlen, pdMS_TO_TICKS(100));

        frame_count++;
        if (frame_count % 100 == 0) {
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
    (void)arg; (void)base; (void)event_data;
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
    default: break;
    }
}

esp_err_t transcribe_ws_connect(const char *session_id)
{
    char uri[256];
    build_ws_uri(uri, sizeof(uri), "/ws/transcribe", session_id);
    ESP_LOGI(TAG, "connecting %s", uri);

    esp_websocket_client_config_t cfg = {
        .uri         = uri,
        .buffer_size = 8192,
        .task_stack  = 8192,
        .task_prio   = 5,
        .skip_cert_common_name_check = true,
    };
    s_ws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(s_ws);

    // Wait up to 5s for connection
    for (int i = 0; i < 50; i++) {
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
        feed_task, "transcribe_feed", 8 * 1024, NULL, 5,
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
    // Wait for feed task to exit (up to 3s)
    for (int i = 0; i < 30 && s_feed_task_handle != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    pipeline_ws_recorder_close();
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    ESP_LOGI(TAG, "[OK] disconnected");
    return ESP_OK;
}
