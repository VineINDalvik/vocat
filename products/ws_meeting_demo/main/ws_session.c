// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "ws_session.h"
#include "api_client.h"
#include "transcribe_ws.h"
#include "host_ws.h"
#include "mp3_player.h"
#include "pipeline_ws.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lwip/ip4_addr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

static const char *TAG = "ws_session";

// Play a short sine-wave beep: freq Hz, duration_ms milliseconds
static void play_beep(int freq_hz, int duration_ms)
{
    int total_frames = 16000 * duration_ms / 1000;
    int16_t *buf = malloc((size_t)total_frames * sizeof(int16_t));
    if (!buf) return;
    for (int i = 0; i < total_frames; i++) {
        float t = (float)i / 16000.0f;
        // Envelope: 10ms fade-in/out to avoid clicks
        float env = 1.0f;
        int fade = 16000 * 10 / 1000;  // 10ms
        if (i < fade) env = (float)i / fade;
        else if (i > total_frames - fade) env = (float)(total_frames - i) / fade;
        buf[i] = (int16_t)(sinf(2.0f * (float)M_PI * freq_hz * t) * 16000.0f * env);
    }
    pipeline_ws_player_write_pcm(buf, total_frames);
    free(buf);
}

volatile bool g_mic_muted = false;

#define WAKE_WORD "开始演示样机"

static bool text_contains_wake_word(const char *text)
{
    if (!text) return false;
    return strstr(text, "开始演示样机") != NULL;
}

typedef enum {
    CMD_START, CMD_STOP, CMD_ENTER_HOST, CMD_EXIT_HOST, CMD_RECREATE_SESSION_HOST, CMD_INTERRUPT, CMD_TOGGLE,
} cmd_type_t;

typedef struct { cmd_type_t type; } session_cmd_t;

static QueueHandle_t      s_cmd_queue = NULL;
static ws_session_state_t s_state          = WS_SESSION_IDLE;
static int64_t            s_last_state_us  = 0;   // time of last set_state()
static char               s_session_id[64] = {0};
static uint32_t           s_answer_chunk_count = 0;
static bool               s_first_answer_text_logged = false;
volatile bool      s_interrupted = false;
volatile bool      s_skip_cooldown = false;

// Forward declarations (needed because on_host_msg calls enqueue_cmd)
static void enqueue_cmd(cmd_type_t type);

// ---------------------------------------------------------------------------
// State transitions
// ---------------------------------------------------------------------------
static void set_state(ws_session_state_t st)
{
    s_state = st;
    s_last_state_us = esp_timer_get_time();
    static const char *names[] = {"IDLE","CONNECTING","MEETING","HOST","ERROR"};
    ESP_LOGI(TAG, "state → %s", names[(int)st]);
    ESP_LOGI(TAG, "heap_free=%lu", esp_get_free_heap_size());
}

// ---------------------------------------------------------------------------
// Status updates (no display on ReSpeaker — serial log only)
// ---------------------------------------------------------------------------
void ws_session_update_ui_status(const char *text)
{
    ESP_LOGI(TAG, "status: \"%s\"", text);
}

void ws_session_update_ui_action(const char *text)
{
    ESP_LOGI(TAG, "action: \"%s\"", text);
}

// ---------------------------------------------------------------------------
// Wake word callback from transcribe_ws: "clare" detected in listen mode
// ---------------------------------------------------------------------------
static void on_wake_word_listen_mode(void *ctx)
{
    (void)ctx;
    if (s_state == WS_SESSION_MEETING) {
        ESP_LOGI(TAG, "[WAKE] \"%s\" detected in listen mode → entering host mode", WAKE_WORD);
        enqueue_cmd(CMD_ENTER_HOST);
    }
}

// ---------------------------------------------------------------------------
// Host WS message callback
// ---------------------------------------------------------------------------
static void on_host_msg(const char *type, cJSON *root, void *ctx)
{
    (void)ctx;

    if (strcmp(type, "transcription") == 0) {
        s_interrupted = false;
        cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring) {
            ESP_LOGI(TAG, "[LATENCY] transcription_recv ts=%lldms text=\"%.60s\"",
                     (long long)(esp_timer_get_time() / 1000), text->valuestring);
            // Wake word in host mode → exit host (debounced: only once per utterance)
            static bool s_exit_host_debounced = false;
            if (text_contains_wake_word(text->valuestring)) {
                if (!s_exit_host_debounced) {
                    s_exit_host_debounced = true;
                    ESP_LOGI(TAG, "[WAKE] \"%s\" in host mode → exit host", WAKE_WORD);
                    enqueue_cmd(CMD_EXIT_HOST);
                }
            } else {
                s_exit_host_debounced = false;
            }
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
    set_state(WS_SESSION_CONNECTING);

    if (!wait_for_ip(30000)) {
        ESP_LOGE(TAG, "[FAIL] start_meeting: no network");
        set_state(WS_SESSION_IDLE);
        return;
    }

    if (api_client_create_session(NULL, s_session_id, sizeof(s_session_id)) != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] start_meeting: session create failed");
        set_state(WS_SESSION_IDLE);
        return;
    }

    mp3_player_open();

    // Register wake word callback before connecting transcribe WS
    transcribe_ws_set_wake_word_cb(on_wake_word_listen_mode, NULL);

    if (transcribe_ws_connect(s_session_id) != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] start_meeting: transcribe WS connect failed");
        set_state(WS_SESSION_ERROR);
        ws_session_update_ui_status("Connection Error");
        return;
    }

    set_state(WS_SESSION_MEETING);
    ws_session_update_ui_status("[Meeting - Listen Mode]");
}

static void do_stop_meeting(void)
{
    transcribe_ws_send_end();
    transcribe_ws_disconnect();
    api_client_end_session(s_session_id);
    mp3_player_close();
    memset(s_session_id, 0, sizeof(s_session_id));
    set_state(WS_SESSION_IDLE);
}

static void enqueue_cmd(cmd_type_t type)
{
    if (!s_cmd_queue) return;
    session_cmd_t cmd = {.type = type};
    xQueueSend(s_cmd_queue, &cmd, 0);
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

    transcribe_ws_disconnect();

    host_ws_set_rejected_cb(on_host_session_rejected, NULL);
    host_ws_set_callback(on_host_msg, NULL);
    if (host_ws_connect(s_session_id) != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] enter_host: WS connect failed");
        transcribe_ws_connect(s_session_id);
        ws_session_update_ui_status("[Meeting - Listen Mode]");
        return;
    }

    set_state(WS_SESSION_HOST);
    ws_session_update_ui_status("Listening...");
    // "ding" — entering host mode; mute mic to prevent beep echo from being sent as audio
    g_mic_muted = true;
    play_beep(880, 120);
    vTaskDelay(pdMS_TO_TICKS(300));
    g_mic_muted = false;
}

static void do_recreate_session_host(void)
{
    ESP_LOGI(TAG, "session rejected — ending old session and creating new one");
    host_ws_disconnect();
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
    ESP_LOGI(TAG, "state → MEETING (exit host mode → listen mode)");
    host_ws_disconnect();
    mp3_player_close();
    mp3_player_open();

    set_state(WS_SESSION_MEETING);

    // "dong" — exiting host mode, back to listen
    play_beep(440, 120);
    vTaskDelay(pdMS_TO_TICKS(500));  // wait for beep echo to fade before starting transcribe

    // Re-register wake word callback for listen mode
    transcribe_ws_set_wake_word_cb(on_wake_word_listen_mode, NULL);

    if (transcribe_ws_connect(s_session_id) != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] exit_host: transcribe WS reconnect failed");
        ws_session_update_ui_status("Connection Error");
    } else {
        ws_session_update_ui_status("[Meeting - Listen Mode]");
    }
}

static void do_interrupt(void)
{
    if (s_state != WS_SESSION_HOST) {
        ESP_LOGW(TAG, "interrupt ignored — not in HOST state");
        return;
    }
    ESP_LOGI(TAG, "interrupt: stopping AI response");

    mp3_player_stop();
    pipeline_ws_player_reset();
    host_ws_send_interrupt();
    host_ws_force_resume();
    s_skip_cooldown = true;
    s_interrupted = true;
    ws_session_update_ui_status("Interrupt - Listening...");
}

// ---------------------------------------------------------------------------
// BOOT button (GPIO9) — toggle host/listen mode
// ---------------------------------------------------------------------------
#define BTN_GPIO       GPIO_NUM_0

static void button_poll_task(void *arg)
{
    int low_count = 0;
    int64_t last_press = 0;
    bool prev_level = true;
    uint32_t tick = 0;
    ESP_LOGI(TAG, "[BTN-TASK] started, monitoring GPIO%d", (int)BTN_GPIO);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));
        tick++;
        bool level = (bool)gpio_get_level(BTN_GPIO);
        // Heartbeat every 5s so we know the task is alive
        if (tick % 250 == 0) {
            ESP_LOGI(TAG, "[BTN-TASK] alive tick=%lu GPIO%d=%s",
                     (unsigned long)tick, (int)BTN_GPIO, level ? "H" : "L");
        }
        if (level != prev_level) {
            ESP_LOGI(TAG, "[BTN-DBG] GPIO%d → %s", (int)BTN_GPIO, level ? "HIGH" : "LOW");
            prev_level = level;
        }
        if (!level) {
            low_count++;
        } else {
            low_count = 0;
        }
        if (low_count == 3) {
            int64_t now = esp_timer_get_time();
            if (now - last_press > 1500000) {
                last_press = now;
                ESP_LOGI(TAG, "[BTN] BOOT button pressed — toggle mode");
                enqueue_cmd(CMD_TOGGLE);
            }
        }
    }
}

void ws_session_btn_init(void)
{
    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);
    xTaskCreatePinnedToCoreWithCaps(
        button_poll_task, "btn_poll", 2 * 1024, NULL, 3,
        NULL, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "BOOT button (GPIO%d) configured — press to toggle mode", BTN_GPIO);
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
        case CMD_TOGGLE: {
            // Guard: ignore if state was entered too recently (absorbs GPIO9 noise and
            // queued toggles that accumulated while CMD_START was running)
            int64_t now_us = esp_timer_get_time();
            if (now_us - s_last_state_us < 3000000) {
                ESP_LOGW(TAG, "toggle ignored — state too recent (%lldms ago)",
                         (long long)((now_us - s_last_state_us) / 1000));
                break;
            }
            if (s_state == WS_SESSION_MEETING) {
                do_enter_host();
            } else if (s_state == WS_SESSION_HOST) {
                do_exit_host();
            } else {
                ESP_LOGW(TAG, "toggle ignored — state=%d", (int)s_state);
            }
            break;
        }
        }
    }
}

// ---------------------------------------------------------------------------
// Initialization
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
esp_err_t ws_session_interrupt(void)      { enqueue_cmd(CMD_INTERRUPT);  return ESP_OK; }
ws_session_state_t ws_session_get_state(void) { return s_state; }