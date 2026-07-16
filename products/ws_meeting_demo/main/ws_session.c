// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "ws_session.h"
#include "api_client.h"
#include "transcribe_ws.h"
#include "host_ws.h"
#include "mp3_player.h"
#include "pipeline_ws.h"
#include "ui_meeting.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lwip/ip4_addr.h"
#include "lvgl.h"
#include "bsp/esp_vocat.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "ws_session";

volatile bool g_mic_muted = false;

typedef enum {
    CMD_START, CMD_STOP, CMD_ENTER_HOST, CMD_EXIT_HOST, CMD_RECREATE_SESSION_HOST, CMD_INTERRUPT,
} cmd_type_t;

typedef struct { cmd_type_t type; } session_cmd_t;

static QueueHandle_t      s_cmd_queue = NULL;
static volatile ws_session_state_t s_state = WS_SESSION_IDLE;
static volatile bool      s_start_pending = false;
static char               s_session_id[64] = {0};
static uint32_t           s_answer_chunk_count = 0;
static bool               s_first_answer_text_logged = false;
volatile bool      s_interrupted = false;
volatile bool      s_skip_cooldown = false;

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

// Thread-safe UI action text update (for function calling status)
static void do_ui_action_update(void *param)
{
    ui_update_t *u = (ui_update_t *)param;
    ui_meeting_set_action_text(u->text);
    ESP_LOGI(TAG, "ui action: \"%.40s\"", u->text);
    free(u);
}

void ws_session_update_ui_action(const char *text)
{
    ui_update_t *u = malloc(sizeof(ui_update_t));
    if (!u) return;
    strlcpy(u->text, text, sizeof(u->text));
    lv_async_call(do_ui_action_update, u);
}

// ---------------------------------------------------------------------------
// Host WS message callback
// ---------------------------------------------------------------------------
static void on_host_msg(const char *type, cJSON *root, void *ctx)
{
    (void)ctx;

    if (strcmp(type, "transcription") == 0) {
        s_interrupted = false;  // new speech recognized — old response definitely over
        cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring) {
            ESP_LOGI(TAG, "[LATENCY] transcription_recv ts=%lldms text=\"%.60s\"",
                     (long long)(esp_timer_get_time() / 1000), text->valuestring);
        }

    } else if (strcmp(type, "answer_text") == 0) {
        cJSON *done_j = cJSON_GetObjectItemCaseSensitive(root, "done");
        cJSON *text_j = cJSON_GetObjectItemCaseSensitive(root, "text");
        bool done = cJSON_IsTrue(done_j);
        int tlen = cJSON_IsString(text_j) ? (int)strlen(text_j->valuestring) : 0;
        if (!s_first_answer_text_logged && tlen > 0) {
            ESP_LOGI(TAG, "[LATENCY] first_answer_text_recv ts=%lldms",
                     (long long)(esp_timer_get_time() / 1000));
            s_first_answer_text_logged = true;
        }
        if (done) {
            ESP_LOGI(TAG, "[LATENCY] answer_text_done ts=%lldms",
                     (long long)(esp_timer_get_time() / 1000));
        }

    } else if (strcmp(type, "answer_audio") == 0) {
        if (s_interrupted) {
            ESP_LOGI(TAG, "discarding answer_audio (interrupt active)");
            return;
        }
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
                    s_answer_chunk_count++;
                    int64_t now_ms = esp_timer_get_time() / 1000;
                    ESP_LOGI(TAG, "[LATENCY] answer_audio_recv chunk #%lu len=%u ts=%lldms heap=%lu",
                             (unsigned long)s_answer_chunk_count, (unsigned)out_len,
                             (long long)now_ms, esp_get_free_heap_size());
                    mp3_player_enqueue(mp3, out_len);
                } else {
                    ESP_LOGW(TAG, "base64 decode failed rc=%d out_len=%u", rc, (unsigned)out_len);
                }
                free(mp3);
            } else {
                ESP_LOGE(TAG, "malloc(%u) failed for answer_audio, heap=%lu",
                         (unsigned)mp3_max, esp_get_free_heap_size());
            }
        }

    } else if (strcmp(type, "function_call") == 0) {
        cJSON *name_j = cJSON_GetObjectItemCaseSensitive(root, "name");
        cJSON *args_j = cJSON_GetObjectItemCaseSensitive(root, "arguments");
        if (cJSON_IsString(name_j) && name_j->valuestring) {
            char *args_str = args_j ? cJSON_PrintUnformatted(args_j) : NULL;
            ESP_LOGI(TAG, "function_call: name=\"%s\" args=%s",
                     name_j->valuestring, args_str ? args_str : "(null)");
            if (args_str) cJSON_free(args_str);
            char buf[256];
            snprintf(buf, sizeof(buf), "%s...", name_j->valuestring);
            ws_session_update_ui_action(buf);
        }

    } else if (strcmp(type, "function_result") == 0) {
        cJSON *name_j    = cJSON_GetObjectItemCaseSensitive(root, "name");
        cJSON *success_j = cJSON_GetObjectItemCaseSensitive(root, "success");
        cJSON *msg_j     = cJSON_GetObjectItemCaseSensitive(root, "message");
        const char *fname = (cJSON_IsString(name_j) && name_j->valuestring)
                            ? name_j->valuestring : "?";
        bool success = cJSON_IsTrue(success_j);
        const char *msg = (cJSON_IsString(msg_j) && msg_j->valuestring)
                          ? msg_j->valuestring : "";
        ESP_LOGI(TAG, "function_result: name=\"%s\" success=%s message=\"%s\"",
                 fname, success ? "true" : "false", msg);
        char buf[256];
        if (success) {
            snprintf(buf, sizeof(buf), "OK %s", msg);
        } else {
            snprintf(buf, sizeof(buf), "Error: %s", msg);
        }
        ws_session_update_ui_action(buf);

    } else if (strcmp(type, "error") == 0) {
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (cJSON_IsString(msg) && msg->valuestring) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Error: %s", msg->valuestring);
            ws_session_update_ui_status(buf);
        }
        // Force VAD resume so s_sending doesn't stay false forever
        host_ws_force_resume();
    } else if (strcmp(type, "interrupt_ack") == 0) {
        s_interrupted = false;
        ESP_LOGI(TAG, "interrupt_ack received — resuming normal flow");
    } else if (strcmp(type, "done") == 0) {
        s_interrupted = false;
        s_answer_chunk_count = 0;
        s_first_answer_text_logged = false;
        ESP_LOGI(TAG, "[LATENCY] done_recv ts=%lldms — signaling mp3 player",
                 (long long)(esp_timer_get_time() / 1000));
        mp3_player_signal_done();
    }
}

// ---------------------------------------------------------------------------
// Wait until the default netif has a non-zero IP (up to timeout_ms)
// ---------------------------------------------------------------------------
static bool wait_for_ip(uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        esp_netif_t *netif = esp_netif_get_default_netif();
        if (netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
                !ip4_addr_isany_val(ip_info.ip)) {
                ESP_LOGI(TAG, "IP ready: " IPSTR, IP2STR(&ip_info.ip));
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        elapsed += 500;
        if (elapsed % 5000 == 0) {
            ESP_LOGW(TAG, "waiting for IP... %lus", (unsigned long)(elapsed / 1000));
        }
    }
    ESP_LOGE(TAG, "[FAIL] no IP after %lums", (unsigned long)timeout_ms);
    return false;
}

// ---------------------------------------------------------------------------
// State machine actions (run in session_cmd_task — blocking allowed)
// ---------------------------------------------------------------------------
static void do_start_meeting(void)
{
    s_start_pending = false;
    if (s_state != WS_SESSION_IDLE) {
        ESP_LOGW(TAG, "start_meeting ignored in state=%d", (int)s_state);
        return;
    }
    set_state(WS_SESSION_CONNECTING);

    if (!pipeline_ws_hw_is_ready()) {
        ESP_LOGE(TAG, "[FAIL] start_meeting: audio hardware not ready");
        set_state(WS_SESSION_IDLE);
        ws_session_update_ui_status("Hardware Not Ready");
        return;
    }
    ESP_LOGI(TAG, "[OK] audio hardware ready");
    ws_session_update_ui_status("Checking WiFi...");

    if (!wait_for_ip(30000)) {
        ESP_LOGE(TAG, "[FAIL] start_meeting: no network");
        set_state(WS_SESSION_IDLE);
        ws_session_update_ui_status("WiFi Not Connected");
        return;
    }

    ws_session_update_ui_status("Connecting meeting server...");
    if (api_client_check_reachable() != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] start_meeting: backend unreachable");
        set_state(WS_SESSION_IDLE);
        ws_session_update_ui_status("Meeting Server Unreachable");
        return;
    }

    ws_session_update_ui_status("Creating meeting session...");
    if (api_client_create_session(NULL, s_session_id, sizeof(s_session_id)) != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] start_meeting: session create failed");
        set_state(WS_SESSION_IDLE);
        ws_session_update_ui_status("Meeting Service Error");
        return;
    }

    ws_session_update_ui_status("Starting audio...");
    if (mp3_player_open() != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] start_meeting: audio player open failed");
        api_client_end_session(s_session_id);
        memset(s_session_id, 0, sizeof(s_session_id));
        set_state(WS_SESSION_IDLE);
        ws_session_update_ui_status("Audio Start Failed");
        return;
    }

    ws_session_update_ui_status("Connecting live transcription...");
    if (transcribe_ws_connect(s_session_id) != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] start_meeting: transcribe WS connect failed");
        mp3_player_close();
        api_client_end_session(s_session_id);
        memset(s_session_id, 0, sizeof(s_session_id));
        set_state(WS_SESSION_IDLE);
        ws_session_update_ui_status("Meeting Connection Failed");
        return;
    }

    set_state(WS_SESSION_MEETING);
    ui_meeting_set_state(UI_STATE_MEETING);
    ws_session_update_ui_status("[Meeting]");
}

static void do_stop_meeting(void)
{
    transcribe_ws_send_end();
    transcribe_ws_disconnect();
    api_client_end_session(s_session_id);
    mp3_player_close();
    memset(s_session_id, 0, sizeof(s_session_id));
    set_state(WS_SESSION_IDLE);
    ui_meeting_set_state(UI_STATE_IDLE);
}

static bool enqueue_cmd(cmd_type_t type)
{
    if (!s_cmd_queue) return false;
    session_cmd_t cmd = {.type = type};
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

static void on_host_session_rejected(void *ctx)
{
    (void)ctx;
    enqueue_cmd(CMD_RECREATE_SESSION_HOST);
}

static void do_enter_host(void)
{
    ESP_LOGI(TAG, "state → HOST (entering host mode)");
    ws_session_update_ui_status("Connecting...");

    // Disconnect transcribe first — it owns the recorder (singleton).
    // Must stop its feed task and close recorder before host can open it.
    transcribe_ws_disconnect();

    host_ws_set_rejected_cb(on_host_session_rejected, NULL);
    host_ws_set_callback(on_host_msg, NULL);
    if (host_ws_connect(s_session_id) != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] enter_host: WS connect failed");
        transcribe_ws_connect(s_session_id);
        ws_session_update_ui_status("[Meeting]");
        return;
    }

    set_state(WS_SESSION_HOST);
    ui_meeting_set_state(UI_STATE_HOST);
    ws_session_update_ui_status("Listening...");
}

static void do_recreate_session_host(void)
{
    ESP_LOGI(TAG, "session rejected — ending old session and creating new one");
    host_ws_disconnect();  // clean up the rejected WS client before reconnecting
    api_client_end_session(s_session_id);
    memset(s_session_id, 0, sizeof(s_session_id));

    if (api_client_create_session(NULL, s_session_id, sizeof(s_session_id)) != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] recreate_session_host: session create failed");
        set_state(WS_SESSION_ERROR);
        ws_session_update_ui_status("Connection Error");
        return;
    }
    ESP_LOGI(TAG, "[OK] new session: %s", s_session_id);
    do_enter_host();
}

static void do_exit_host(void)
{
    ESP_LOGI(TAG, "state → MEETING (exit host mode)");
    host_ws_disconnect();
    mp3_player_close();
    mp3_player_open();

    set_state(WS_SESSION_MEETING);
    ui_meeting_set_state(UI_STATE_MEETING);

    if (transcribe_ws_connect(s_session_id) != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] exit_host: transcribe WS reconnect failed");
        ws_session_update_ui_status("Connection Error");
    } else {
        ws_session_update_ui_status("[Meeting]");
    }
}

static void do_interrupt(void)
{
    if (s_state != WS_SESSION_HOST) {
        ESP_LOGW(TAG, "interrupt ignored — not in HOST state");
        return;
    }
    if (!g_mic_muted && !mp3_player_pending()) {
        ESP_LOGI(TAG, "interrupt ignored — AI not speaking");
        return;
    }
    ESP_LOGI(TAG, "interrupt: stopping AI response");

    // 1. Stop mp3 player — unmute mic, drain queue, reset decoder state
    mp3_player_stop();

    // 2. Reset pipeline player ring buffer — discard pending PCM
    pipeline_ws_player_reset();

    // 3. Send interrupt to server — stop generating TTS
    host_ws_send_interrupt();

    // 4. Force resume VAD — start listening for new question (skip cooldown)
    host_ws_force_resume();
    s_skip_cooldown = true;

    // 5. Mark session as interrupted — discard straggler answer_audio
    s_interrupted = true;

    // 6. UI feedback
    ws_session_update_ui_status("Interrupt - Listening...");
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
        case CMD_START:                  do_start_meeting();          break;
        case CMD_STOP:                   do_stop_meeting();           break;
        case CMD_ENTER_HOST:             do_enter_host();             break;
        case CMD_EXIT_HOST:              do_exit_host();              break;
        case CMD_RECREATE_SESSION_HOST:  do_recreate_session_host();  break;
        case CMD_INTERRUPT:              do_interrupt();              break;
        }
    }
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
esp_err_t ws_session_start_meeting(void)
{
    if (s_start_pending || s_state != WS_SESSION_IDLE) {
        ESP_LOGW(TAG, "duplicate Start ignored");
        return ESP_ERR_INVALID_STATE;
    }
    s_start_pending = true;
    if (!enqueue_cmd(CMD_START)) {
        s_start_pending = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t ws_session_stop_meeting(void)   { enqueue_cmd(CMD_STOP);       return ESP_OK; }
esp_err_t ws_session_enter_host(void)     { enqueue_cmd(CMD_ENTER_HOST); return ESP_OK; }
esp_err_t ws_session_exit_host(void)      { enqueue_cmd(CMD_EXIT_HOST);  return ESP_OK; }
esp_err_t ws_session_interrupt(void)      { enqueue_cmd(CMD_INTERRUPT);  return ESP_OK; }
ws_session_state_t ws_session_get_state(void) { return s_state; }
