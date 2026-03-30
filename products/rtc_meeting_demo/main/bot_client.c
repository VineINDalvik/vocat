// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "bot_client.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "sdkconfig.h"

static const char *TAG = "bot_client";

// Response buffer size — 2KB, enough for the credential JSON
#define RESP_BUF_SIZE  2048

// ---------------------------------------------------------------------------
// HTTP helper — POST JSON, return response body in caller-allocated buffer
// ---------------------------------------------------------------------------

typedef struct {
    char   *buf;
    int     buf_size;
    int     data_len;
} http_resp_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
        resp->data_len = 0;
        break;
    case HTTP_EVENT_ON_DATA:
        if (resp && evt->data_len > 0) {
            int remaining = resp->buf_size - resp->data_len - 1; // -1 for NUL
            if (remaining > 0) {
                int copy_len = (evt->data_len < remaining) ? evt->data_len : remaining;
                memcpy(resp->buf + resp->data_len, evt->data, copy_len);
                resp->data_len += copy_len;
                resp->buf[resp->data_len] = '\0';
            } else {
                ESP_LOGW(TAG, "Response buffer full, truncating");
            }
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/**
 * Perform a POST request with JSON body and Authorization header.
 * Response body is written into resp->buf (NUL-terminated).
 * Returns ESP_OK only if HTTP status 200 received.
 */
static esp_err_t do_post(const char *url, const char *body,
                          const char *auth_header, http_resp_t *resp)
{
    esp_http_client_config_t cfg = {
        .url                   = url,
        .method                = HTTP_METHOD_POST,
        .event_handler         = http_event_handler,
        .user_data             = resp,
        .timeout_ms            = 10000,
        .skip_cert_common_name_check = true,
        // TLS cert verify is disabled globally via sdkconfig.defaults
        // (CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y)
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d (expected 200)", status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t bot_client_start_chat(rtc_room_info_t *info)
{
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));

    // Build Authorization header: "af78e30" + APPID
    char auth[80];
    int ret = snprintf(auth, sizeof(auth), "af78e30%s", CONFIG_RTC_APPID);
    if (ret >= (int)sizeof(auth)) {
        ESP_LOGE(TAG, "auth buffer overflow");
        return ESP_FAIL;
    }

    // Build URL
    char url[128];
    ret = snprintf(url, sizeof(url), "http://%s/startvoicechat", CONFIG_AIGENT_SERVER_HOST);
    if (ret >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "url buffer overflow");
        return ESP_FAIL;
    }

    // Build request body
    cJSON *req = cJSON_CreateObject();
    if (!req) {
        ESP_LOGE(TAG, "cJSON_CreateObject failed");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(req, "audio_codec", "OPUS");
    // no room_identifier → room_id = OPUS{uuid}, use default AIGC policy group
    cJSON_AddBoolToObject(req, "enable_burst", false);
    cJSON_AddNumberToObject(req, "asr_type", 1);           // bigmodel ASR
    cJSON_AddBoolToObject(req, "tts_is_bidirection", true); // volcano_bidirection TTS
    cJSON_AddBoolToObject(req, "enable_conversation_state_callback", true); // LISTENING/THINKING/ANSWERING
    // tts_is_bidirection: false (default volcano TTS, simpler and works out of box)
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted failed");
        return ESP_FAIL;
    }

    // Allocate response buffer on heap
    char *resp_buf = malloc(RESP_BUF_SIZE);
    if (!resp_buf) {
        free(body);
        ESP_LOGE(TAG, "Response buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    resp_buf[0] = '\0';

    http_resp_t resp = {
        .buf      = resp_buf,
        .buf_size = RESP_BUF_SIZE,
        .data_len = 0,
    };

    ESP_LOGI(TAG, "POST %s  body=%s", url, body);
    esp_err_t err = do_post(url, body, auth, &resp);
    free(body);

    if (err != ESP_OK) {
        free(resp_buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Response: %s", resp_buf);

    // Parse JSON response
    // Expected: {"code":200,"msg":"","data":{"room_id":"...","uid":"...","app_id":"...",
    //            "token":"...","task_id":"...","bot_uid":"..."}}
    cJSON *root = cJSON_Parse(resp_buf);
    free(resp_buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_FAIL;
    }

    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(data)) {
        ESP_LOGE(TAG, "Missing 'data' object in response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    // Helper macro to extract a string field
#define EXTRACT_STR(field, dest, dest_size) \
    do { \
        cJSON *item = cJSON_GetObjectItemCaseSensitive(data, field); \
        if (!cJSON_IsString(item) || !item->valuestring) { \
            ESP_LOGE(TAG, "Missing or invalid field '%s'", field); \
            cJSON_Delete(root); \
            return ESP_FAIL; \
        } \
        strncpy(dest, item->valuestring, (dest_size) - 1); \
        dest[(dest_size) - 1] = '\0'; \
    } while (0)

    EXTRACT_STR("room_id",  info->room_id,  sizeof(info->room_id));
    EXTRACT_STR("uid",      info->uid,      sizeof(info->uid));
    EXTRACT_STR("app_id",   info->app_id,   sizeof(info->app_id));
    EXTRACT_STR("task_id",  info->task_id,  sizeof(info->task_id));
    EXTRACT_STR("bot_uid",  info->bot_uid,  sizeof(info->bot_uid));
    EXTRACT_STR("token",    info->token,    sizeof(info->token));

#undef EXTRACT_STR

    cJSON_Delete(root);

    ESP_LOGI(TAG, "start_chat OK: room_id=%s uid=%s app_id=%s task_id=%s bot_uid=%s",
             info->room_id, info->uid, info->app_id, info->task_id, info->bot_uid);
    return ESP_OK;
}

esp_err_t bot_client_stop_chat(const rtc_room_info_t *info)
{
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }

    // Build Authorization header
    char auth[80];
    int ret = snprintf(auth, sizeof(auth), "af78e30%s", CONFIG_RTC_APPID);
    if (ret >= (int)sizeof(auth)) {
        ESP_LOGE(TAG, "auth buffer overflow");
        return ESP_FAIL;
    }

    // Build URL
    char url[128];
    ret = snprintf(url, sizeof(url), "http://%s/stopvoicechat", CONFIG_AIGENT_SERVER_HOST);
    if (ret >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "url buffer overflow");
        return ESP_FAIL;
    }

    // Build request body: app_id + room_id + task_id
    cJSON *req = cJSON_CreateObject();
    if (!req) {
        ESP_LOGE(TAG, "cJSON_CreateObject failed");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(req, "app_id",  info->app_id);
    cJSON_AddStringToObject(req, "room_id", info->room_id);
    cJSON_AddStringToObject(req, "task_id", info->task_id);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted failed");
        return ESP_FAIL;
    }

    // Allocate response buffer on heap
    char *resp_buf = malloc(RESP_BUF_SIZE);
    if (!resp_buf) {
        free(body);
        ESP_LOGE(TAG, "Response buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    resp_buf[0] = '\0';

    http_resp_t resp = {
        .buf      = resp_buf,
        .buf_size = RESP_BUF_SIZE,
        .data_len = 0,
    };

    ESP_LOGI(TAG, "POST %s  body=%s", url, body);
    esp_err_t err = do_post(url, body, auth, &resp);
    free(body);
    free(resp_buf);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stop_chat request failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "stop_chat OK: room_id=%s task_id=%s", info->room_id, info->task_id);
    return ESP_OK;
}
