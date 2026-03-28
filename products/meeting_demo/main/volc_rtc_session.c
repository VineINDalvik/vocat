// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "volc_rtc_session.h"
#include "pipeline_gmf.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "volc_conv_ai.h"
#include "sdkconfig.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"

static const char *TAG = "volc_rtc_session";

// ---------------------------------------------------------------------------
// Volc engine JSON config template
// Fields: instance_id, product_key, product_secret, device_name
// ---------------------------------------------------------------------------
// Config format matches official example (conv_ai_embedded_kit.c).
// Required fields: video.codec (without it RTC init fails with "read video_codec failed")
// and params array for RTC tuning.
#define VOLC_CONFIG_FMT \
    "{\"ver\":1," \
     "\"iot\":{" \
       "\"instance_id\":\"%s\"," \
       "\"product_key\":\"%s\"," \
       "\"product_secret\":\"%s\"," \
       "\"device_name\":\"%s\"" \
     "}," \
     "\"rtc\":{" \
       "\"log_level\":1," \
       "\"audio\":{\"publish\":true,\"subscribe\":true,\"codec\":3}," \
       "\"video\":{\"publish\":false,\"subscribe\":false,\"codec\":1}," \
       "\"params\":[" \
           "\"{\\\"debug\\\":{\\\"log_to_console\\\":1}}\"," \
           "\"{\\\"audio\\\":{\\\"codec\\\":{\\\"internal\\\":{\\\"enable\\\":1}}}}\"," \
           "\"{\\\"rtc\\\":{\\\"access\\\":{\\\"concurrent_requests\\\":1}}}\"," \
           "\"{\\\"rtc\\\":{\\\"ice\\\":{\\\"concurrent_agents\\\":1}}}\"" \
       "]" \
     "}" \
    "}"

// ---------------------------------------------------------------------------
// Audio feed task config
// ---------------------------------------------------------------------------
#define FEED_TASK_STACK    (8 * 1024)   // 8KB in PSRAM
#define FEED_TASK_PRIORITY (5)
#define FEED_TASK_CORE     (1)

// volc G711A channel expects 8kHz 16bit mono PCM.
// We read 16kHz from pipeline then downsample 2:1.
// 16kHz 20ms frame = 640 bytes; downsampled 8kHz 20ms = 320 bytes.
#define AUDIO_FRAME_16K_BYTES  PIPELINE_FRAME_BYTES   // 640 bytes at 16kHz
#define AUDIO_FRAME_BYTES      (PIPELINE_FRAME_BYTES / 2)  // 320 bytes at 8kHz

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
typedef struct {
    volc_engine_t            engine;
    volc_session_state_t     state;
    volc_session_state_cb_t  cb;
    void                    *cb_ctx;
    TaskHandle_t             feed_task;
    TaskHandle_t             caller_task;  // notified when feed task exits
    volatile bool            running;
    volatile bool            starting;     // guard against double-start race
    volatile bool            mic_muted;    // true while agent is speaking (no AEC workaround)
} session_t;

static session_t s_sess = {
    .engine      = NULL,
    .state       = VOLC_SESSION_IDLE,
    .cb          = NULL,
    .cb_ctx      = NULL,
    .feed_task   = NULL,
    .caller_task = NULL,
    .running     = false,
    .starting    = false,
    .mic_muted   = false,
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void set_state(volc_session_state_t new_state)
{
    if (s_sess.state == new_state) {
        return;
    }
    s_sess.state = new_state;
    ESP_LOGI(TAG, "state → %d", (int)new_state);
    if (s_sess.cb) {
        s_sess.cb(new_state, s_sess.cb_ctx);
    }
}

// ---------------------------------------------------------------------------
// Volc SDK callbacks
// ---------------------------------------------------------------------------

static void on_volc_event(volc_engine_t handle, volc_event_t *event, void *user_data)
{
    switch (event->code) {
    case VOLC_EV_CONNECTED:
        ESP_LOGI(TAG, "VOLC_EV_CONNECTED");
        set_state(VOLC_SESSION_CONNECTED);
        break;
    case VOLC_EV_DISCONNECTED:
        ESP_LOGI(TAG, "VOLC_EV_DISCONNECTED");
        set_state(VOLC_SESSION_IDLE);
        break;
    case VOLC_EV_QUOTA_EXCEEDED:
        ESP_LOGW(TAG, "VOLC_EV_QUOTA_EXCEEDED");
        set_state(VOLC_SESSION_ERROR);
        break;
    default:
        ESP_LOGI(TAG, "volc event %d", (int)event->code);
        break;
    }
}

static void on_volc_conv_status(volc_engine_t handle, volc_conv_status_e status, void *user_data)
{
    const char *names[] = {"?", "LISTENING", "THINKING", "ANSWERING", "INTERRUPTED", "ANSWER_FINISH"};
    int idx = (status >= 1 && status <= 5) ? (int)status : 0;
    ESP_LOGI(TAG, "conv status: %s", names[idx]);

    // No AEC workaround: mute mic while agent is speaking to prevent echo feedback.
    switch (status) {
    case VOLC_CONV_STATUS_ANSWERING:
        if (!s_sess.mic_muted) {
            s_sess.mic_muted = true;
            ESP_LOGI(TAG, "mic MUTED (agent speaking)");
        }
        break;
    case VOLC_CONV_STATUS_ANSWER_FINISH:
    case VOLC_CONV_STATUS_LISTENING:
    case VOLC_CONV_STATUS_INTERRUPTED:
        if (s_sess.mic_muted) {
            s_sess.mic_muted = false;
            ESP_LOGI(TAG, "mic UNMUTED (user turn)");
        }
        break;
    default:
        break;
    }
    ESP_LOGI(TAG, "mic_muted=%d after conv_status=%s", (int)s_sess.mic_muted, names[idx]);
}

// Timestamp of last received audio frame from Agent (ms since boot)
static volatile int64_t s_last_agent_audio_ms = 0;
// How long after Agent stops speaking before unmuting mic (ms)
#define AGENT_SILENCE_UNMUTE_MS  600

static void on_volc_audio_data(volc_engine_t handle,
                                const void *data, size_t data_len,
                                volc_audio_frame_info_t *info, void *user_data)
{
    static uint32_t s_audio_cb_count = 0;
    s_audio_cb_count++;
    if (s_audio_cb_count == 1 || s_audio_cb_count % 200 == 0) {
        ESP_LOGI(TAG, "audio_data cb #%lu: data_type=%d len=%d",
                 (unsigned long)s_audio_cb_count,
                 info ? (int)info->data_type : -1,
                 (int)data_len);
    }
    if (s_sess.running) {
        // Mute mic whenever Agent is speaking (echo prevention without AEC)
        s_last_agent_audio_ms = esp_timer_get_time() / 1000;
        if (!s_sess.mic_muted) {
            s_sess.mic_muted = true;
        }
        pipeline_gmf_player_write(data, data_len);
    }
}

static void on_volc_video_data(volc_engine_t handle,
                                const void *data, size_t data_len,
                                volc_video_frame_info_t *info, void *user_data)
{
    // Not used in audio-only mode
}

static void on_volc_message_data(volc_engine_t handle,
                                  const void *data, size_t data_len,
                                  volc_message_info_t *info, void *user_data)
{
    // Subtitle / function-call messages — log for now
    if (data_len > 8) {
        ESP_LOGD(TAG, "message (len=%d): %.4s…", (int)data_len, (const char *)data);
    }
}

// ---------------------------------------------------------------------------
// Audio feed task: microphone → volc engine
// ---------------------------------------------------------------------------

// Simple 2:1 downsampler: pick every other sample (16kHz → 8kHz)
static void downsample_2to1(const int16_t *src, int16_t *dst, int src_frames)
{
    for (int i = 0; i < src_frames / 2; i++) {
        dst[i] = src[i * 2];
    }
}

static void audio_feed_task(void *arg)
{
    ESP_LOGI(TAG, "audio feed task started (16kHz→8kHz downsample)");

    // Read buffer at 16kHz, downsample to 8kHz for volc
    int16_t *buf_16k = heap_caps_malloc(AUDIO_FRAME_16K_BYTES, MALLOC_CAP_DEFAULT);
    int16_t *buf_8k  = heap_caps_malloc(AUDIO_FRAME_BYTES, MALLOC_CAP_DEFAULT);
    if (!buf_16k || !buf_8k) {
        ESP_LOGE(TAG, "feed buf alloc failed");
        heap_caps_free(buf_16k);
        heap_caps_free(buf_8k);
        vTaskDelete(NULL);
        return;
    }

    volc_audio_frame_info_t frame_info = {
        .data_type = VOLC_AUDIO_DATA_TYPE_PCM,
        .commit    = false,
    };

    uint32_t send_count = 0;
    while (s_sess.running) {
        int n = pipeline_gmf_recorder_read(buf_16k, AUDIO_FRAME_16K_BYTES);
        if (n != AUDIO_FRAME_16K_BYTES) {
            ESP_LOGW(TAG, "recorder_read returned %d (expected %d)", n, AUDIO_FRAME_16K_BYTES);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // Downsample 16kHz → 8kHz (required by G711A channel)
        int frames_16k = AUDIO_FRAME_16K_BYTES / sizeof(int16_t);
        downsample_2to1(buf_16k, buf_8k, frames_16k);

        if (s_sess.state == VOLC_SESSION_CONNECTED && s_sess.engine) {
            // Auto-unmute mic when Agent has been silent for AGENT_SILENCE_UNMUTE_MS
            if (s_sess.mic_muted && s_last_agent_audio_ms > 0) {
                int64_t now_ms = esp_timer_get_time() / 1000;
                if ((now_ms - s_last_agent_audio_ms) >= AGENT_SILENCE_UNMUTE_MS) {
                    s_sess.mic_muted = false;
                    ESP_LOGI(TAG, "mic UNMUTED (agent silent >%dms)", AGENT_SILENCE_UNMUTE_MS);
                }
            }

            if (!s_sess.mic_muted) {
                // Compute RMS to verify mic is picking up actual audio
                if (send_count % 50 == 0) {
                    int64_t sum = 0;
                    const int16_t *s = (const int16_t *)buf_8k;
                    int n = AUDIO_FRAME_BYTES / 2;
                    for (int i = 0; i < n; i++) sum += (int64_t)s[i] * s[i];
                    int rms = (int)(__builtin_sqrt((double)(sum / n)));
                    ESP_LOGI(TAG, "mic RMS=%d (frame #%lu)", rms, (unsigned long)send_count);
                }

                int ret = volc_send_audio_data(s_sess.engine, buf_8k, AUDIO_FRAME_BYTES, &frame_info);
                send_count++;
                if (send_count % 200 == 0) {
                    ESP_LOGI(TAG, "mic sent #%lu (muted=%d ret=%d)",
                             (unsigned long)send_count, (int)s_sess.mic_muted, ret);
                }
                if (ret != VOLC_ERR_NO_ERROR) {
                    ESP_LOGW(TAG, "volc_send_audio_data error %d", ret);
                }
            }
        }
    }

    heap_caps_free(buf_16k);
    heap_caps_free(buf_8k);
    ESP_LOGI(TAG, "audio feed task exiting");

    // Notify stop caller that we are done before self-deleting
    if (s_sess.caller_task) {
        xTaskNotify(s_sess.caller_task, 1, eSetValueWithOverwrite);
    }
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t volc_rtc_session_start(const char *bot_id,
                                  volc_session_state_cb_t cb,
                                  void *ctx)
{
    // Double-start guard: reject if already running OR start in progress
    if (s_sess.running || s_sess.starting) {
        ESP_LOGW(TAG, "session already running or starting");
        return ESP_ERR_INVALID_STATE;
    }
    s_sess.starting = true;

    ESP_RETURN_ON_FALSE(bot_id && strlen(bot_id) > 0, ESP_ERR_INVALID_ARG,
                        TAG, "bot_id must not be empty");

    s_sess.cb        = cb;
    s_sess.cb_ctx    = ctx;
    s_sess.mic_muted = false;   // always reset at session start

    // ---- Attach TLS CA bundle so volc SDK can verify HTTPS certs ----
    esp_tls_init_global_ca_store();
    esp_crt_bundle_attach(NULL);

    // ---- Wait up to 5s for WiFi to be connected ----
    for (int i = 0; i < 50; i++) {
        esp_netif_t *netif = esp_netif_get_default_netif();
        esp_netif_ip_info_t ip;
        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            break;
        }
        ESP_LOGI(TAG, "Waiting for WiFi... (%d/50)", i + 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // ---- Init audio hardware ----
    ESP_RETURN_ON_ERROR(pipeline_gmf_hw_init(),       TAG, "hw_init failed");
    ESP_RETURN_ON_ERROR(pipeline_gmf_recorder_open(), TAG, "recorder_open failed");
    ESP_RETURN_ON_ERROR(pipeline_gmf_player_open(),   TAG, "player_open failed");

    set_state(VOLC_SESSION_CONNECTING);

    // ---- Build config JSON ----
    char config_buf[1024];
    snprintf(config_buf, sizeof(config_buf), VOLC_CONFIG_FMT,
             CONFIG_MEETING_VOLC_INSTANCE_ID,
             CONFIG_MEETING_VOLC_APP_ID,      // used as product_key
             CONFIG_MEETING_VOLC_TOKEN,        // used as product_secret
             "meeting_demo_device");           // fixed device name for demo

    // ---- Create engine ----
    volc_event_handler_t handlers = {
        .on_volc_event               = on_volc_event,
        .on_volc_conversation_status = on_volc_conv_status,
        .on_volc_audio_data          = on_volc_audio_data,
        .on_volc_video_data          = on_volc_video_data,
        .on_volc_message_data        = on_volc_message_data,
    };
    int ret = volc_create(&s_sess.engine, config_buf, &handlers, NULL);
    if (ret != VOLC_ERR_NO_ERROR) {
        ESP_LOGE(TAG, "volc_create failed: %d (%s)", ret, volc_err_2_str(ret));
        pipeline_gmf_recorder_close();
        pipeline_gmf_player_close();
        s_sess.starting = false;
        set_state(VOLC_SESSION_ERROR);
        return ESP_FAIL;
    }

    // ---- Start engine (async — VOLC_EV_CONNECTED arrives via callback) ----
    volc_opt_t opt = {
        .mode   = VOLC_MODE_RTC,
        .bot_id = (char *)bot_id,
        .params = NULL,
    };
    ret = volc_start(s_sess.engine, &opt);
    if (ret != VOLC_ERR_NO_ERROR) {
        ESP_LOGE(TAG, "volc_start failed: %d (%s)", ret, volc_err_2_str(ret));
        volc_destroy(s_sess.engine);
        s_sess.engine = NULL;
        pipeline_gmf_recorder_close();
        pipeline_gmf_player_close();
        s_sess.starting = false;
        set_state(VOLC_SESSION_ERROR);
        return ESP_FAIL;
    }

    // ---- Launch audio feed task (stack in PSRAM) ----
    s_sess.running = true;
    s_sess.caller_task = NULL;  // will be set in stop()
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        audio_feed_task, "volc_feed",
        FEED_TASK_STACK, NULL,
        FEED_TASK_PRIORITY, &s_sess.feed_task,
        FEED_TASK_CORE);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create feed task");
        s_sess.running = false;
        volc_stop(s_sess.engine);
        volc_destroy(s_sess.engine);
        s_sess.engine = NULL;
        pipeline_gmf_recorder_close();
        pipeline_gmf_player_close();
        s_sess.starting = false;
        set_state(VOLC_SESSION_ERROR);
        return ESP_ERR_NO_MEM;
    }

    s_sess.starting = false;
    ESP_LOGI(TAG, "Session started for bot_id=%s", bot_id);
    return ESP_OK;
}

esp_err_t volc_rtc_session_stop(void)
{
    if (!s_sess.running && s_sess.engine == NULL) {
        return ESP_OK;   // already stopped, idempotent
    }

    ESP_LOGI(TAG, "Stopping session...");

    // 1. Register ourselves as the task to notify, then signal feed task to exit
    s_sess.caller_task = xTaskGetCurrentTaskHandle();
    s_sess.running = false;

    // 2. Wait up to 3s for feed task to notify us before self-deleting
    if (s_sess.feed_task) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
        s_sess.feed_task   = NULL;
        s_sess.caller_task = NULL;
    }

    // 3. Stop and destroy volc engine
    if (s_sess.engine) {
        volc_stop(s_sess.engine);
        volc_destroy(s_sess.engine);
        s_sess.engine = NULL;
    }

    // 4. Close audio pipeline
    pipeline_gmf_recorder_close();
    pipeline_gmf_player_close();
    pipeline_gmf_hw_deinit();

    s_sess.mic_muted = false;
    set_state(VOLC_SESSION_IDLE);
    ESP_LOGI(TAG, "Session stopped");
    return ESP_OK;
}

volc_session_state_t volc_rtc_session_get_state(void)
{
    return s_sess.state;
}
