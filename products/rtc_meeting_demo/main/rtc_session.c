// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "rtc_session.h"
#include "bot_client.h"
#include "pipeline_gmf.h"
#include "VolcEngineRTCLite.h"
#include "esp_opus_enc.h"
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "rtc_session";

#define FEED_TASK_STACK    (10 * 1024)
#define FEED_TASK_PRIORITY (5)
#define FEED_TASK_CORE     (1)

typedef struct {
    byte_rtc_engine_t        engine;
    rtc_room_info_t          room_info;
    rtc_session_state_t      state;
    rtc_session_state_cb_t   cb;
    void                    *cb_ctx;
    TaskHandle_t             feed_task;
    TaskHandle_t             caller_task;
    volatile bool            running;
    volatile bool            starting;
    volatile bool            fini_done;  // set by on_fini_notify
} session_t;

static session_t s_sess = { 0 };

/* -------------------------------------------------------------------------- */
/* Helper                                                                       */
/* -------------------------------------------------------------------------- */

static void set_state(rtc_session_state_t new_state) {
    if (s_sess.state == new_state) return;
    s_sess.state = new_state;
    ESP_LOGI(TAG, "state → %d", (int)new_state);
    if (s_sess.cb) s_sess.cb(new_state, s_sess.cb_ctx);
}

/* -------------------------------------------------------------------------- */
/* byte_rtc callbacks                                                           */
/* -------------------------------------------------------------------------- */

static void on_join_room_success(byte_rtc_engine_t engine, const char *room, int elapsed_ms, bool rejoin) {
    ESP_LOGI(TAG, "joined room %s in %d ms (rejoin=%d)", room, elapsed_ms, (int)rejoin);
    set_state(RTC_SESSION_CONNECTED);
}

static void on_room_error(byte_rtc_engine_t engine, const char *room, int code, const char *msg) {
    ESP_LOGE(TAG, "room error %s code=%d msg=%s", room, code, msg ? msg : "");
    set_state(RTC_SESSION_ERROR);
}

static void on_fini_notify(byte_rtc_engine_t engine) {
    ESP_LOGI(TAG, "on_fini_notify");
    s_sess.fini_done = true;
}

static void on_quota_exceeded(byte_rtc_engine_t engine, const char *message, const char *extra) {
    ESP_LOGW(TAG, "quota exceeded: %s", message ? message : "");
    set_state(RTC_SESSION_ERROR);
}

// on_audio_data: RTC delivers Opus-encoded packets.
// The 2-byte length prefix expected by ADF raw_opus_decoder is NOT present here.
// We forward the raw Opus packet directly to pipeline_gmf_player_write().
static void on_audio_data(byte_rtc_engine_t engine, const char *room, const char *uid,
                          uint16_t sent_ts, audio_data_type_e codec,
                          const void *data_ptr, size_t data_len,
                          const uint8_t *extra_info, size_t extra_info_size) {
    if (s_sess.running) {
        pipeline_gmf_player_write(data_ptr, data_len);
    }
}

static void on_message_received(byte_rtc_engine_t engine, const char *room, const char *src,
                                const uint8_t *message, int size, bool binary) {
    // Subtitle / function-call messages - log prefix for diagnosis
    if (size > 8) {
        ESP_LOGD(TAG, "message from %s (len=%d): %.4s", src, size, (const char *)message);
    }
}

/* -------------------------------------------------------------------------- */
/* Audio feed task                                                              */
/* -------------------------------------------------------------------------- */

static void audio_feed_task(void *arg) {
    ESP_LOGI(TAG, "audio feed task started");

    // Opus encoder: 16kHz mono 32kbps 20ms frames
    void *enc_hd = NULL;
    esp_opus_enc_cfg_t enc_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc_cfg.sample_rate     = 16000;
    enc_cfg.channel         = 1;   // ESP_AUDIO_MONO = 1
    enc_cfg.bits_per_sample = 16;  // ESP_AUDIO_BIT16 = 16
    enc_cfg.bitrate         = 32000;
    enc_cfg.frame_duration  = ESP_OPUS_ENC_FRAME_DURATION_20_MS;

    esp_audio_err_t err = esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &enc_hd);
    if (err != ESP_AUDIO_ERR_OK || !enc_hd) {
        ESP_LOGE(TAG, "esp_opus_enc_open failed: %d", err);
        if (s_sess.caller_task) xTaskNotify(s_sess.caller_task, 1, eSetValueWithOverwrite);
        vTaskDelete(NULL);
        return;
    }

    // PCM input: 640 bytes (320 samples × 2 bytes, 16kHz 20ms mono)
    int16_t *pcm_buf  = heap_caps_malloc(PIPELINE_FRAME_BYTES, MALLOC_CAP_DEFAULT);
    // Opus output: up to 256 bytes for 32kbps 20ms
    uint8_t *opus_buf = heap_caps_malloc(256, MALLOC_CAP_DEFAULT);
    if (!pcm_buf || !opus_buf) {
        ESP_LOGE(TAG, "feed buf alloc failed");
        esp_opus_enc_close(enc_hd);
        heap_caps_free(pcm_buf);
        heap_caps_free(opus_buf);
        if (s_sess.caller_task) xTaskNotify(s_sess.caller_task, 1, eSetValueWithOverwrite);
        vTaskDelete(NULL);
        return;
    }

    audio_frame_info_t frame_info = { .data_type = AUDIO_DATA_TYPE_OPUS };

    while (s_sess.running) {
        int n = pipeline_gmf_recorder_read(pcm_buf, PIPELINE_FRAME_BYTES);
        if (n != PIPELINE_FRAME_BYTES) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (s_sess.state != RTC_SESSION_CONNECTED || !s_sess.engine) {
            continue;
        }

        // Encode PCM → Opus
        esp_audio_enc_in_frame_t  in  = { .buffer = (uint8_t *)pcm_buf, .len = PIPELINE_FRAME_BYTES };
        esp_audio_enc_out_frame_t out = { .buffer = opus_buf,            .len = 256 };
        err = esp_opus_enc_process(enc_hd, &in, &out);
        if (err != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "opus encode error %d", err);
            continue;
        }

        if (out.encoded_bytes > 0) {
            byte_rtc_send_audio_data(s_sess.engine, s_sess.room_info.room_id,
                                     opus_buf, out.encoded_bytes, &frame_info);
        }
    }

    esp_opus_enc_close(enc_hd);
    heap_caps_free(pcm_buf);
    heap_caps_free(opus_buf);
    ESP_LOGI(TAG, "audio feed task exiting");

    if (s_sess.caller_task) {
        xTaskNotify(s_sess.caller_task, 1, eSetValueWithOverwrite);
    }
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/* Engine destroy task (dispatched to avoid blocking the SDK callback thread)  */
/* -------------------------------------------------------------------------- */

static void engine_destroy_task(void *arg) {
    byte_rtc_engine_t engine = (byte_rtc_engine_t)arg;
    byte_rtc_fini(engine);
    // Wait for fini_notify via a polling loop (safe on a separate task)
    for (int i = 0; i < 50 && !s_sess.fini_done; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    byte_rtc_destroy(engine);
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                   */
/* -------------------------------------------------------------------------- */

esp_err_t rtc_session_start(rtc_session_state_cb_t cb, void *ctx) {
    if (s_sess.running || s_sess.starting) {
        ESP_LOGW(TAG, "session already running or starting");
        return ESP_ERR_INVALID_STATE;
    }
    s_sess.starting  = true;
    s_sess.cb        = cb;
    s_sess.cb_ctx    = ctx;
    s_sess.fini_done = false;

    // Wait up to 5s for WiFi
    for (int i = 0; i < 50; i++) {
        esp_netif_t *netif = esp_netif_get_default_netif();
        esp_netif_ip_info_t ip;
        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) break;
        ESP_LOGI(TAG, "Waiting for WiFi... (%d/50)", i + 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Warn if WiFi is still not connected after waiting
    {
        esp_netif_t *check_netif = esp_netif_get_default_netif();
        esp_netif_ip_info_t check_ip;
        if (!check_netif || esp_netif_get_ip_info(check_netif, &check_ip) != ESP_OK || check_ip.ip.addr == 0) {
            ESP_LOGW(TAG, "WiFi not connected after 5s, proceeding anyway");
        }
    }

    // Init audio hardware — use explicit checks so we can clean up on failure
    if (pipeline_gmf_hw_init() != ESP_OK) {
        ESP_LOGE(TAG, "hw_init failed");
        goto fail_clean;
    }
    if (pipeline_gmf_recorder_open() != ESP_OK) {
        ESP_LOGE(TAG, "recorder_open failed");
        pipeline_gmf_hw_deinit();
        goto fail_clean;
    }
    if (pipeline_gmf_player_open() != ESP_OK) {
        ESP_LOGE(TAG, "player_open failed");
        pipeline_gmf_recorder_close();
        pipeline_gmf_hw_deinit();
        goto fail_clean;
    }

    set_state(RTC_SESSION_CONNECTING);

    // HTTP: get room credentials
    if (bot_client_start_chat(&s_sess.room_info) != ESP_OK) {
        ESP_LOGE(TAG, "bot_client_start_chat failed");
        pipeline_gmf_recorder_close();
        pipeline_gmf_player_close();
        pipeline_gmf_hw_deinit();
        s_sess.starting = false;
        set_state(RTC_SESSION_ERROR);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Room: %s, UID: %s", s_sess.room_info.room_id, s_sess.room_info.uid);

    // Create byte_rtc engine
    byte_rtc_event_handler_t handlers = {
        .on_join_room_success = on_join_room_success,
        .on_room_error        = on_room_error,
        .on_audio_data        = on_audio_data,
        .on_message_received  = on_message_received,
        .on_fini_notify       = on_fini_notify,
        .on_quota_exceeded    = on_quota_exceeded,
    };
    s_sess.engine = byte_rtc_create(s_sess.room_info.app_id, &handlers);
    if (!s_sess.engine) {
        ESP_LOGE(TAG, "byte_rtc_create failed");
        goto fail_after_http;
    }

    byte_rtc_set_log_level(s_sess.engine, BYTE_RTC_LOG_LEVEL_ERROR);
    int init_ret = byte_rtc_init(s_sess.engine);
    if (init_ret != 0) {
        ESP_LOGE(TAG, "byte_rtc_init failed: %d", init_ret);
        goto fail_after_engine;
    }
    byte_rtc_set_audio_codec(s_sess.engine, AUDIO_CODEC_TYPE_OPUS);

    // Join room
    byte_rtc_room_options_t opts = {
        .auto_subscribe_audio = true,
        .auto_subscribe_video = false,
        .auto_publish_audio   = true,
        .auto_publish_video   = false,
    };
    if (byte_rtc_join_room(s_sess.engine,
                           s_sess.room_info.room_id,
                           s_sess.room_info.uid,
                           s_sess.room_info.token,
                           &opts) != 0) {
        ESP_LOGE(TAG, "byte_rtc_join_room failed");
        goto fail_after_engine;
    }

    // Launch audio feed task
    s_sess.running     = true;
    s_sess.caller_task = NULL;
    if (xTaskCreatePinnedToCore(audio_feed_task, "rtc_feed",
                                FEED_TASK_STACK, NULL,
                                FEED_TASK_PRIORITY, &s_sess.feed_task,
                                FEED_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "failed to create feed task");
        s_sess.running = false;
        goto fail_after_engine;
    }

    s_sess.starting = false;
    ESP_LOGI(TAG, "Session started");
    return ESP_OK;

fail_after_engine:
    {
        byte_rtc_engine_t engine_to_destroy = s_sess.engine;
        s_sess.engine = NULL;
        // Dispatch fini+destroy to a task so we don't block the SDK callback thread
        xTaskCreate(engine_destroy_task, "rtc_destroy", 4096, engine_to_destroy, 5, NULL);
        // Give the destroy task a moment to start before we clean up pipeline
        vTaskDelay(pdMS_TO_TICKS(200));
    }

fail_after_http:
    bot_client_stop_chat(&s_sess.room_info);
    pipeline_gmf_recorder_close();
    pipeline_gmf_player_close();
    pipeline_gmf_hw_deinit();
    s_sess.starting = false;
    set_state(RTC_SESSION_ERROR);
    return ESP_FAIL;

fail_clean:
    s_sess.starting = false;
    set_state(RTC_SESSION_ERROR);
    return ESP_FAIL;
}

esp_err_t rtc_session_stop(void) {
    if (!s_sess.running && !s_sess.engine) return ESP_OK;

    ESP_LOGI(TAG, "Stopping session...");

    // Stop feed task
    s_sess.caller_task = xTaskGetCurrentTaskHandle();
    s_sess.running = false;
    if (s_sess.feed_task) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
        s_sess.feed_task   = NULL;
        s_sess.caller_task = NULL;
    }

    // Leave room and destroy engine
    if (s_sess.engine) {
        byte_rtc_leave_room(s_sess.engine, s_sess.room_info.room_id);
        vTaskDelay(pdMS_TO_TICKS(500));
        byte_rtc_engine_t engine_to_destroy = s_sess.engine;
        s_sess.engine = NULL;
        // Dispatch fini+destroy to a task so we don't block the SDK callback thread
        xTaskCreate(engine_destroy_task, "rtc_destroy", 4096, engine_to_destroy, 5, NULL);
        // Give the destroy task a moment to start before we clean up pipeline
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // Stop AI agent
    bot_client_stop_chat(&s_sess.room_info);

    // Close audio pipeline
    pipeline_gmf_recorder_close();
    pipeline_gmf_player_close();
    pipeline_gmf_hw_deinit();

    set_state(RTC_SESSION_IDLE);
    ESP_LOGI(TAG, "Session stopped");
    return ESP_OK;
}

rtc_session_state_t rtc_session_get_state(void) {
    return s_sess.state;
}
