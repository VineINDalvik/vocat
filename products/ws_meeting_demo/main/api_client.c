// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "api_client.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "api_client";

// Enough for the session create response
#define RESP_BUF_SIZE 512

// Accumulate HTTP response into a fixed buffer
typedef struct {
    char   buf[RESP_BUF_SIZE];
    int    len;
} resp_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_ctx_t *ctx = (resp_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx) {
        int remaining = RESP_BUF_SIZE - ctx->len - 1;
        if (remaining > 0) {
            int copy = evt->data_len < remaining ? evt->data_len : remaining;
            memcpy(ctx->buf + ctx->len, evt->data, copy);
            ctx->len += copy;
            ctx->buf[ctx->len] = '\0';
        }
    }
    return ESP_OK;
}

esp_err_t api_client_check_reachable(void)
{
    esp_http_client_config_t cfg = {
        .url            = CONFIG_WS_MEETING_API_BASE_URL,
        .method         = HTTP_METHOD_GET,
        .timeout_ms     = 30000,
        .addr_type      = HTTP_ADDR_TYPE_INET,
        .skip_cert_common_name_check = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "[FAIL] connectivity check: http client init failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    int tls_error = 0;
    int tls_flags = 0;
    esp_http_client_get_and_clear_last_tls_error(client, &tls_error, &tls_flags);
    esp_http_client_cleanup(client);

    // Any HTTP response proves DNS, routing, TCP/TLS, and the configured backend
    // are reachable. The API base path itself may legitimately return 404.
    if (err != ESP_OK || status <= 0) {
        ESP_LOGE(TAG, "[FAIL] connectivity check http_err=%d status=%d tls_err=%d tls_flags=0x%x",
                 (int)err, status, tls_error, tls_flags);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[OK] backend reachable, status=%d", status);
    return ESP_OK;
}

esp_err_t api_client_create_session(const char *topic,
                                     char *out_session_id, size_t len)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/api/session", CONFIG_WS_MEETING_API_BASE_URL);

    char body[128];
    snprintf(body, sizeof(body), "{\"topic\":\"%s\"}",
             topic ? topic : CONFIG_WS_MEETING_TOPIC);

    resp_ctx_t resp = {0};

    esp_http_client_config_t cfg = {
        .url            = url,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = 30000,
        .addr_type      = HTTP_ADDR_TYPE_INET,
        .event_handler  = http_event_handler,
        .user_data      = &resp,
        .skip_cert_common_name_check = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "[FAIL] create_session: http client init failed");
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "[FAIL] create_session http_err=%d status=%d", (int)err, status);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_ParseWithLength(resp.buf, (size_t)resp.len);
    if (!root) {
        ESP_LOGE(TAG, "[FAIL] create_session: JSON parse error");
        return ESP_FAIL;
    }
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    if (!cJSON_IsString(id) || !id->valuestring) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "[FAIL] create_session: no session_id in response");
        return ESP_FAIL;
    }
    strlcpy(out_session_id, id->valuestring, len);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "[OK] session created: %s", out_session_id);
    ESP_LOGI(TAG, "heap_free=%lu", esp_get_free_heap_size());
    return ESP_OK;
}

esp_err_t api_client_end_session(const char *session_id)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/api/session/%s/end",
             CONFIG_WS_MEETING_API_BASE_URL, session_id);

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .addr_type  = HTTP_ADDR_TYPE_INET,
        .skip_cert_common_name_check = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "[FAIL] end_session: http client init failed");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status == 200) {
        ESP_LOGI(TAG, "[OK] session ended: %s", session_id);
        ESP_LOGI(TAG, "heap_free=%lu", esp_get_free_heap_size());
        return ESP_OK;
    }
    ESP_LOGE(TAG, "[FAIL] end_session http_err=%d status=%d", (int)err, status);
    return ESP_FAIL;
}
