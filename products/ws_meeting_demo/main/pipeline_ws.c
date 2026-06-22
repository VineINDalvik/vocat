// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0
//
// pipeline_ws.c — Audio pipeline for ws_meeting_demo.
// ReSpeaker XVF3800 + XIAO ESP32-S3 adaptation.
// Audio via XMOS I2S (16kHz 32bit 2ch), XMOS handles AEC/beamforming/codec internally.

#include "pipeline_ws.h"
#include "xmos_ctrl.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "sdkconfig.h"

static const char *TAG = "pipeline_ws";

// ---------------------------------------------------------------------------
// ReSpeaker XVF3800 + XIAO ESP32-S3 pin definitions
// ---------------------------------------------------------------------------
#define RS_I2S_BCLK     GPIO_NUM_8
#define RS_I2S_WS       GPIO_NUM_7
#define RS_I2S_DOUT     GPIO_NUM_44      // ESP32 TX → XMOS (reference audio for AEC)
#define RS_I2S_DIN      GPIO_NUM_43      // XMOS → ESP32 (processed mic audio)
#define RS_I2C_SDA      GPIO_NUM_5       // XIAO D5 — I2C to XMOS
#define RS_I2C_SCL      GPIO_NUM_6       // XIAO D6 — I2C to XMOS
#define RS_I2C_PORT     (0)

#define CODEC_SAMPLE_RATE   (16000)
#define CODEC_BITS          (32)
#define CODEC_CHANNELS      (2)

// ---------------------------------------------------------------------------
// Playback ring buffer: 3000ms of 16kHz 16bit mono PCM = 96000 bytes
// ---------------------------------------------------------------------------
#define PLAY_RB_SIZE        (96000)
#define PLAY_PRE_ROLL_BYTES (1600)    // 50ms pre-roll at 16kHz 16bit mono
#define PLAY_TASK_STACK     (8 * 1024)
#define PLAY_TASK_PRIORITY  (7)
#define PLAY_TASK_CORE      (0)

// Recorder: max 100ms frame = 1600 samples = 25600 bytes raw (32bit stereo)
#define REC_RAW_BUF_FRAMES  3200

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static i2c_master_bus_handle_t  s_i2c_bus        = NULL;
static bool                     s_i2c_owned       = false;
static i2s_chan_handle_t         s_i2s_tx         = NULL;
static i2s_chan_handle_t         s_i2s_rx         = NULL;
static bool                     s_hw_inited       = false;
static bool                     s_rec_open        = false;
static bool                     s_play_open       = false;
static int32_t                 *s_rec_raw_buf     = NULL;
static RingbufHandle_t           s_play_rb         = NULL;
static TaskHandle_t              s_play_task       = NULL;
static volatile bool             s_play_task_run   = false;
static SemaphoreHandle_t         s_play_done_sem   = NULL;
static volatile size_t           s_pre_roll_written = 0;
static volatile bool             s_play_drained     = false;
static volatile bool             s_player_reset     = false;

// ---------------------------------------------------------------------------
// Sample format helpers
// ---------------------------------------------------------------------------
static void conv_32s_to_16m(const int32_t *src, int16_t *dst, int frames)
{
    for (int i = 0; i < frames; i++) {
        int32_t r = src[i * 2 + 1] >> 16;
        dst[i] = (int16_t)r;
    }
}

static void conv_16m_to_32s(const int16_t *src, int32_t *dst, int frames)
{
    for (int i = 0; i < frames; i++) {
        int32_t v = (int32_t)src[i] << 16;
        dst[i * 2]     = v;
        dst[i * 2 + 1] = v;
    }
}

// ---------------------------------------------------------------------------
// HW init / deinit
// ---------------------------------------------------------------------------
esp_err_t pipeline_ws_hw_init(void)
{
    if (s_hw_inited) return ESP_OK;

    // I2C bus for XMOS XVF3800 control (volume, mute, LED, DoA)
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port          = RS_I2C_PORT,
        .sda_io_num        = RS_I2C_SDA,
        .scl_io_num        = RS_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c_bus), TAG, "I2C init failed");
    s_i2c_owned = true;

    // Initialize XMOS control interface
    ESP_RETURN_ON_ERROR(xmos_ctrl_init(s_i2c_bus), TAG, "XMOS ctrl init failed");

    // I2S channels — ESP32 as master, 16kHz 32-bit stereo
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(RS_I2C_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, &s_i2s_rx), TAG, "I2S new channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(CODEC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(CODEC_BITS, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC,    // No MCLK — XMOS generates its own clock
            .bclk = RS_I2S_BCLK,
            .ws   = RS_I2S_WS,
            .dout = RS_I2S_DOUT,
            .din  = RS_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg), TAG, "I2S TX init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_rx, &std_cfg), TAG, "I2S RX init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "I2S TX enable failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_rx), TAG, "I2S RX enable failed");
    ESP_LOGI(TAG, "[OK] hw_init: I2S ready %dHz %dbit %dch (ReSpeaker XVF3800)",
             CODEC_SAMPLE_RATE, CODEC_BITS, CODEC_CHANNELS);

    s_hw_inited = true;
    return ESP_OK;
}

esp_err_t pipeline_ws_hw_deinit(void)
{
    if (!s_hw_inited) return ESP_OK;
    xmos_ctrl_deinit();
    if (s_i2s_tx) { i2s_channel_disable(s_i2s_tx); i2s_del_channel(s_i2s_tx); s_i2s_tx = NULL; }
    if (s_i2s_rx) { i2s_channel_disable(s_i2s_rx); i2s_del_channel(s_i2s_rx); s_i2s_rx = NULL; }
    if (s_i2c_owned && s_i2c_bus) { i2c_del_master_bus(s_i2c_bus); s_i2c_owned = false; }
    s_i2c_bus   = NULL;
    s_hw_inited = false;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Recorder — direct I2S read from XMOS
// ---------------------------------------------------------------------------
esp_err_t pipeline_ws_recorder_open(void)
{
    ESP_RETURN_ON_FALSE(s_hw_inited, ESP_ERR_INVALID_STATE, TAG, "HW not initialized");
    ESP_RETURN_ON_FALSE(!s_rec_open, ESP_ERR_INVALID_STATE, TAG, "Recorder already open");
    s_rec_raw_buf = heap_caps_malloc(
        (size_t)REC_RAW_BUF_FRAMES * 2 * sizeof(int32_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_rec_raw_buf, ESP_ERR_NO_MEM, TAG, "rec raw buf alloc failed");
    s_rec_open = true;
    ESP_LOGI(TAG, "[OK] recorder opened");
    return ESP_OK;
}

int pipeline_ws_recorder_read(void *buf, size_t size)
{
    if (!s_rec_open || !s_i2s_rx || !s_rec_raw_buf) return -1;
    int out_frames = (int)(size / sizeof(int16_t));
    if (out_frames > REC_RAW_BUF_FRAMES) {
        ESP_LOGE(TAG, "recorder_read: %d frames > max %d", out_frames, REC_RAW_BUF_FRAMES);
        return -1;
    }
    size_t raw_bytes = (size_t)out_frames * 2 * sizeof(int32_t);
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(s_i2s_rx, s_rec_raw_buf, raw_bytes, &bytes_read, pdMS_TO_TICKS(100));
    if (ret != ESP_OK || bytes_read < raw_bytes) {
        ESP_LOGE(TAG, "I2S read error: ret=%d, got=%zu/%zu", ret, bytes_read, raw_bytes);
        return -1;
    }
    conv_32s_to_16m(s_rec_raw_buf, (int16_t *)buf, out_frames);
    return (int)size;
}

esp_err_t pipeline_ws_recorder_close(void)
{
    if (!s_rec_open) return ESP_OK;
    s_rec_open = false;
    if (s_rec_raw_buf) { heap_caps_free(s_rec_raw_buf); s_rec_raw_buf = NULL; }
    ESP_LOGI(TAG, "[OK] recorder closed");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Playback task — drains ring buffer, writes directly to I2S TX
// ---------------------------------------------------------------------------
static void player_task(void *arg)
{
    int32_t out_buf[320 * 2];   // 20ms I2S frame: 320 stereo 32-bit samples
    int16_t acc_buf[320];
    int     acc_frames  = 0;
    bool    playing     = false;
    int     empty_runs  = 0;

    while (s_play_task_run) {
        if (s_player_reset) {
            s_player_reset = false;
            playing = false;
            acc_frames = 0;
            empty_runs = 0;
            s_pre_roll_written = 0;
            s_play_drained = true;
            size_t drain_size;
            uint8_t *drain_item;
            while ((drain_item = (uint8_t *)xRingbufferReceiveUpTo(
                    s_play_rb, &drain_size, pdMS_TO_TICKS(0), PLAY_RB_SIZE)) != NULL) {
                vRingbufferReturnItem(s_play_rb, drain_item);
            }
            memset(out_buf, 0, sizeof(out_buf));
            size_t written = 0;
            i2s_channel_write(s_i2s_tx, out_buf, sizeof(out_buf), &written, pdMS_TO_TICKS(20));
            continue;
        }

        if (!playing) {
            if (s_pre_roll_written >= PLAY_PRE_ROLL_BYTES) {
                playing = true;
                empty_runs = 0;
                ESP_LOGI(TAG, "[OK] pre-roll reached %u bytes, starting playback",
                         (unsigned)s_pre_roll_written);
            } else {
                memset(out_buf, 0, sizeof(out_buf));
                size_t written = 0;
                i2s_channel_write(s_i2s_tx, out_buf, sizeof(out_buf), &written, pdMS_TO_TICKS(20));
                continue;
            }
        }

        size_t need = (size_t)(320 - acc_frames) * sizeof(int16_t);
        size_t rx_len = 0;
        int16_t *pcm = (int16_t *)xRingbufferReceiveUpTo(
            s_play_rb, &rx_len, pdMS_TO_TICKS(20), need);

        if (pcm && rx_len > 0) {
            int got = (int)(rx_len / sizeof(int16_t));
            memcpy(acc_buf + acc_frames, pcm, (size_t)got * sizeof(int16_t));
            vRingbufferReturnItem(s_play_rb, pcm);
            acc_frames += got;
            empty_runs = 0;
        }

        if (acc_frames >= 320) {
            conv_16m_to_32s(acc_buf, out_buf, 320);
            size_t written = 0;
            i2s_channel_write(s_i2s_tx, out_buf, sizeof(out_buf), &written, pdMS_TO_TICKS(20));
            acc_frames = 0;
            continue;
        }

        if (pcm == NULL || rx_len == 0) {
            empty_runs++;
            if (acc_frames > 0) {
                conv_16m_to_32s(acc_buf, out_buf, acc_frames);
                memset(out_buf + acc_frames * 2, 0,
                       (size_t)(320 - acc_frames) * 2 * sizeof(int32_t));
                size_t written = 0;
                i2s_channel_write(s_i2s_tx, out_buf, sizeof(out_buf), &written, pdMS_TO_TICKS(20));
                acc_frames = 0;
            } else {
                memset(out_buf, 0, sizeof(out_buf));
                size_t written = 0;
                i2s_channel_write(s_i2s_tx, out_buf, sizeof(out_buf), &written, pdMS_TO_TICKS(20));
            }
            if (empty_runs >= 5) {
                s_pre_roll_written = 0;
                s_play_drained = true;
                playing = false;
                empty_runs = 0;
            }
        }
    }

    if (s_play_done_sem) xSemaphoreGive(s_play_done_sem);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Player public API
// ---------------------------------------------------------------------------
esp_err_t pipeline_ws_player_open(void)
{
    ESP_RETURN_ON_FALSE(s_hw_inited,  ESP_ERR_INVALID_STATE, TAG, "HW not initialized");
    ESP_RETURN_ON_FALSE(!s_play_open, ESP_ERR_INVALID_STATE, TAG, "Player already open");

    s_play_rb = xRingbufferCreate(PLAY_RB_SIZE, RINGBUF_TYPE_BYTEBUF);
    ESP_RETURN_ON_FALSE(s_play_rb, ESP_ERR_NO_MEM, TAG, "ring buffer alloc failed");

    s_play_done_sem = xSemaphoreCreateBinary();
    if (!s_play_done_sem) {
        vRingbufferDelete(s_play_rb); s_play_rb = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_pre_roll_written = 0;
    s_play_drained = false;
    s_play_task_run = true;
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        player_task, "play_task", PLAY_TASK_STACK, NULL,
        PLAY_TASK_PRIORITY, &s_play_task, PLAY_TASK_CORE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        vRingbufferDelete(s_play_rb); s_play_rb = NULL;
        vSemaphoreDelete(s_play_done_sem); s_play_done_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_play_open = true;
    ESP_LOGI(TAG, "[OK] player opened (PCM ring buffer → I2S TX)");
    return ESP_OK;
}

esp_err_t pipeline_ws_player_write_pcm(const int16_t *pcm, int frames)
{
    if (!s_play_open || !s_play_rb) return ESP_ERR_INVALID_STATE;
    s_play_drained = false;
    s_pre_roll_written += (size_t)frames * sizeof(int16_t);
    BaseType_t ok = xRingbufferSend(s_play_rb, pcm,
                                     (size_t)frames * sizeof(int16_t),
                                     pdMS_TO_TICKS(200));
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool pipeline_ws_player_is_drained(void)
{
    return s_play_drained;
}

esp_err_t pipeline_ws_player_close(void)
{
    if (!s_play_open) return ESP_OK;
    s_play_task_run = false;
    if (s_play_done_sem) {
        xSemaphoreTake(s_play_done_sem, pdMS_TO_TICKS(3000));
        vSemaphoreDelete(s_play_done_sem); s_play_done_sem = NULL;
    }
    s_play_task = NULL;
    if (s_play_rb) { vRingbufferDelete(s_play_rb); s_play_rb = NULL; }
    s_play_open = false;
    ESP_LOGI(TAG, "[OK] player closed");
    return ESP_OK;
}

esp_err_t pipeline_ws_player_reset(void)
{
    if (!s_play_open || !s_play_rb) return ESP_ERR_INVALID_STATE;
    s_player_reset = true;
    ESP_LOGI(TAG, "[OK] player reset requested");
    return ESP_OK;
}

esp_err_t pipeline_ws_set_volume(int volume)
{
    return xmos_ctrl_set_volume(volume);
}