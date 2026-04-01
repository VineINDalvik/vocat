// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "ws_session.h"
#include "api_client.h"
#include "transcribe_ws.h"
#include "host_ws.h"
#include "mp3_player.h"
#include "ui_meeting.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "bsp/esp_vocat.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "ws_session";

volatile bool g_mic_muted = false;

typedef enum {
    CMD_START, CMD_STOP, CMD_ENTER_HOST, CMD_EXIT_HOST,
} cmd_type_t;

typedef struct { cmd_type_t type; } session_cmd_t;

static QueueHandle_t      s_cmd_queue = NULL;
static ws_session_state_t s_state     = WS_SESSION_IDLE;
static char               s_session_id[64] = {0};

// ---------------------------------------------------------------------------
// State transitions
// ---------------------------------------------------------------------------
static void set_state(ws_session_state_t st)
{
    s_state = st;
    static const char *names[] = {"IDLE","CONNECTING","MEETING","HOST","ERROR"};
    ESP_LOGI(TAG, "state → %s", names[(int)st]);
    ESP_LOGI(TAG, "heap_free=%lu", esp_get_free_heap_size());
}

// ---------------------------------------------------------------------------
// Thread-safe UI update via lv_async_call
// ---------------------------------------------------------------------------
typedef struct { char text[256]; } ui_update_t;

static void do_ui_update(void *param)
{
    ui_update_t *u = (ui_update_t *)param;
    ui_meeting_set_status(u->text);
    ESP_LOGI(TAG, "ui status: \"%.40s\"", u->text);
    free(u);
}

void ws_session_update_ui_status(const char *text)
{
    ui_update_t *u = malloc(sizeof(ui_update_t));
    if (!u) return;
    strlcpy(u->text, text, sizeof(u->text));
    lv_async_call(do_ui_update, u);
}

// ---------------------------------------------------------------------------
// Host WS message callback
// ---------------------------------------------------------------------------
static void on_host_msg(const char *type, cJSON *root, void *ctx)
{
    (void)ctx;

    if (strcmp(type, "transcription") == 0) {
        cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Q: %s", text->valuestring);
            ws_session_update_ui_status(buf);
            ESP_LOGI(TAG, "recv transcription: %s", text->valuestring);
        }

    } else if (strcmp(type, "answer_text") == 0) {
        cJSON *text_j = cJSON_GetObjectItemCaseSensitive(root, "text");
        cJSON *done_j = cJSON_GetObjectItemCaseSensitive(root, "done");
        bool done = cJSON_IsTrue(done_j);
        int tlen = 0;
        if (cJSON_IsString(text_j) && text_j->valuestring) {
            tlen = (int)strlen(text_j->valuestring);
            if (tlen > 0) ws_session_update_ui_status(text_j->valuestring);
        }
        ESP_LOGI(TAG, "recv answer_text (done=%s) len=%d",
                 done ? "true" : "false", tlen);

    } else if (strcmp(type, "answer_audio") == 0) {
        cJSON *data_j = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (cJSON_IsString(data_j) && data_j->valuestring) {
            const char *b64   = data_j->valuestring;
            size_t      b64_n = strlen(b64);
            size_t      mp3_max = b64_n * 3 / 4 + 4;
            uint8_t    *mp3   = malloc(mp3_max);
            if (mp3) {
                size_t out_len = 0;
                int rc = mbedtls_base64_decode(
                    mp3, mp3_max, &out_len,
                    (const unsigned char *)b64, b64_n);
                if (rc == 0 && out_len > 0) {
                    static uint32_t chunk_count = 0;
                    chunk_count++;
                    ESP_LOGI(TAG, "recv answer_audio chunk #%lu len=%u",
                             (unsigned long)chunk_count, (unsigned)out_len);
                    // Log first answer audio latency
                    if (chunk_count == 1) {
                        ESP_LOGI(TAG, "[LATENCY] first_answer_audio_recv ts=%lldms",
                                 (long long)(esp_timer_get_time() / 1000));
                    }
                    mp3_player_enqueue(mp3, out_len);
                }
                free(mp3);
            }
        }

    } else if (strcmp(type, "error") == 0) {
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (cJSON_IsString(msg) && msg->valuestring) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Error: %s", msg->valuestring);
            ws_session_update_ui_status(buf);
        }
    }
}

// ---------------------------------------------------------------------------
// State machine actions (run in session_cmd_task — blocking allowed)
// ---------------------------------------------------------------------------
static void do_start_meeting(void)
{
    set_state(WS_SESSION_CONNECTING);
    if (bsp_display_lock(100)) {
        ui_meeting_set_state(UI_STATE_MEETING);
        bsp_display_unlock();
    }

    if (api_client_create_session(NULL, s_session_id, sizeof(s_session_id)) != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] start_meeting: session create failed");
        set_state(WS_SESSION_IDLE);
        if (bsp_display_lock(100)) {
            ui_meeting_set_state(UI_STATE_IDLE);
            bsp_display_unlock();
        }
        return;
    }

    mp3_player_open();

    if (transcribe_ws_connect(s_session_id) != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] start_meeting: transcribe WS connect failed");
        set_state(WS_SESSION_ERROR);
        ws_session_update_ui_status("Connection Error");
        return;
    }

    set_state(WS_SESSION_MEETING);
}

static void do_stop_meeting(void)
{
    transcribe_ws_send_end();
    transcribe_ws_disconnect();
    api_client_end_session(s_session_id);
    mp3_player_close();
    memset(s_session_id, 0, sizeof(s_session_id));
    set_state(WS_SESSION_IDLE);
    if (bsp_display_lock(100)) {
        ui_meeting_set_state(UI_STATE_IDLE);
        bsp_display_unlock();
    }
}

static void do_enter_host(void)
{
    ESP_LOGI(TAG, "state → HOST (entering host mode)");
    transcribe_ws_disconnect();

    host_ws_set_callback(on_host_msg, NULL);
    if (host_ws_connect(s_session_id) != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] enter_host: WS connect failed");
        // Reconnect transcribe WS and stay in MEETING
        transcribe_ws_connect(s_session_id);
        set_state(WS_SESSION_MEETING);
        return;
    }

    set_state(WS_SESSION_HOST);
    if (bsp_display_lock(100)) {
        ui_meeting_set_state(UI_STATE_HOST);
        bsp_display_unlock();
    }
    ws_session_update_ui_status("Listening...");
}

static void do_exit_host(void)
{
    ESP_LOGI(TAG, "state → MEETING (exit host mode)");
    host_ws_disconnect();
    mp3_player_close();
    mp3_player_open();

    set_state(WS_SESSION_MEETING);
    if (bsp_display_lock(100)) {
        ui_meeting_set_state(UI_STATE_MEETING);
        bsp_display_unlock();
    }

    if (transcribe_ws_connect(s_session_id) != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] exit_host: transcribe WS reconnect failed");
        ws_session_update_ui_status("Connection Error");
    }
}

// ---------------------------------------------------------------------------
// Command task
// ---------------------------------------------------------------------------
static void session_cmd_task(void *arg)
{
    while (1) {
        session_cmd_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        switch (cmd.type) {
        case CMD_START:      do_start_meeting(); break;
        case CMD_STOP:       do_stop_meeting();  break;
        case CMD_ENTER_HOST: do_enter_host();    break;
        case CMD_EXIT_HOST:  do_exit_host();     break;
        }
    }
}

static void enqueue_cmd(cmd_type_t type)
{
    if (!s_cmd_queue) return;
    session_cmd_t cmd = {.type = type};
    xQueueSend(s_cmd_queue, &cmd, 0);
}

// ---------------------------------------------------------------------------
// Initialization — runs before app_main via constructor
// ---------------------------------------------------------------------------
static void __attribute__((constructor)) ws_session_init(void)
{
    s_cmd_queue = xQueueCreate(4, sizeof(session_cmd_t));
    xTaskCreatePinnedToCoreWithCaps(
        session_cmd_task, "sess_cmd", 8 * 1024, NULL, 4,
        NULL, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
esp_err_t ws_session_start_meeting(void)  { enqueue_cmd(CMD_START);      return ESP_OK; }
esp_err_t ws_session_stop_meeting(void)   { enqueue_cmd(CMD_STOP);       return ESP_OK; }
esp_err_t ws_session_enter_host(void)     { enqueue_cmd(CMD_ENTER_HOST); return ESP_OK; }
esp_err_t ws_session_exit_host(void)      { enqueue_cmd(CMD_EXIT_HOST);  return ESP_OK; }
ws_session_state_t ws_session_get_state(void) { return s_state; }
