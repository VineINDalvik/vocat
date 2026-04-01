// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0
//
// pipeline_ws.c — Audio pipeline for ws_meeting_demo.
// Based on pipeline_gmf.c; Opus decoder replaced with raw PCM ring buffer player.

#include "pipeline_ws.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "es7210_adc.h"
#include "bsp/esp_vocat.h"
#include "sdkconfig.h"

extern i2c_master_bus_handle_t bsp_i2c_get_handle(void);

static const char *TAG = "pipeline_ws";

// ---------------------------------------------------------------------------
// Hardware pin constants (from ESP-VoCat BSP)
// ---------------------------------------------------------------------------
#define CODEC_I2C_PORT      BSP_I2C_NUM
#define CODEC_I2C_SDA       BSP_I2C_SDA
#define CODEC_I2C_SCL       BSP_I2C_SCL
#define CODEC_I2S_PORT      (0)
#define CODEC_I2S_MCLK      BSP_I2S_MCLK
#define CODEC_I2S_BCLK      BSP_I2S_SCLK
#define CODEC_I2S_WS        BSP_I2S_LCLK
#define CODEC_I2S_DOUT      BSP_I2S_DOUT
#define CODEC_I2S_DIN       BSP_I2S_DSIN

#define ES8311_I2C_ADDR     (0x30)
#define ES7210_I2C_ADDR     (0x80)

#define CODEC_SAMPLE_RATE   (16000)
#define CODEC_BITS          (32)
#define CODEC_CHANNELS      (2)

// ---------------------------------------------------------------------------
// Playback ring buffer: 500ms of 16kHz 16bit mono PCM = 16000 bytes
// ---------------------------------------------------------------------------
#define PLAY_RB_SIZE        (16000)
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
static esp_codec_dev_handle_t   s_play_dev        = NULL;
static esp_codec_dev_handle_t   s_rec_dev         = NULL;
static bool                     s_hw_inited       = false;
static bool                     s_rec_open        = false;
static bool                     s_play_open       = false;
static int32_t                 *s_rec_raw_buf     = NULL;
static RingbufHandle_t           s_play_rb         = NULL;
static TaskHandle_t              s_play_task       = NULL;
static volatile bool             s_play_task_run   = false;
static SemaphoreHandle_t         s_play_done_sem   = NULL;

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
// HW init / deinit (unchanged from pipeline_gmf)
// ---------------------------------------------------------------------------

esp_err_t pipeline_ws_hw_init(void)
{
    if (s_hw_inited) return ESP_OK;

    s_i2c_bus = bsp_i2c_get_handle();
    if (!s_i2c_bus) {
        i2c_master_bus_config_t i2c_cfg = {
            .i2c_port          = CODEC_I2C_PORT,
            .sda_io_num        = CODEC_I2C_SDA,
            .scl_io_num        = CODEC_I2C_SCL,
            .clk_source        = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c_bus), TAG, "I2C init failed");
        s_i2c_owned = true;
    } else {
        s_i2c_owned = false;
    }

#ifdef BSP_POWER_CODEC_EN
    gpio_set_direction(BSP_POWER_CODEC_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(BSP_POWER_CODEC_EN, 1);
#endif
    gpio_set_direction(BSP_POWER_AMP_IO, GPIO_MODE_OUTPUT);
    gpio_set_level(BSP_POWER_AMP_IO, 1);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CODEC_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, &s_i2s_rx), TAG, "I2S new channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(CODEC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(CODEC_BITS, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = CODEC_I2S_MCLK,
            .bclk = CODEC_I2S_BCLK,
            .ws   = CODEC_I2S_WS,
            .dout = CODEC_I2S_DOUT,
            .din  = CODEC_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg), TAG, "I2S TX init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_rx, &std_cfg), TAG, "I2S RX init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "I2S TX enable failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_rx), TAG, "I2S RX enable failed");
    ESP_LOGI(TAG, "[OK] hw_init: I2S ready %dHz %dbit %dch",
             CODEC_SAMPLE_RATE, CODEC_BITS, CODEC_CHANNELS);

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = CODEC_I2S_PORT, .tx_handle = s_i2s_tx, .rx_handle = s_i2s_rx,
    };
    const audio_codec_data_if_t *i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(i2s_data_if, ESP_FAIL, TAG, "I2S data interface failed");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    // ES8311 (DAC)
    audio_codec_i2c_cfg_t i2c_es8311 = {
        .port = CODEC_I2C_PORT, .addr = ES8311_I2C_ADDR, .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *es8311_ctrl = audio_codec_new_i2c_ctrl(&i2c_es8311);
    ESP_RETURN_ON_FALSE(es8311_ctrl, ESP_FAIL, TAG, "ES8311 ctrl interface failed");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = es8311_ctrl, .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO, .pa_reverted = false,
        .master_mode = false, .use_mclk = true,
    };
    const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(es8311_if, ESP_FAIL, TAG, "ES8311 codec_new failed");

    esp_codec_dev_cfg_t play_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es8311_if, .data_if = i2s_data_if,
    };
    s_play_dev = esp_codec_dev_new(&play_dev_cfg);
    ESP_RETURN_ON_FALSE(s_play_dev, ESP_FAIL, TAG, "ES8311 dev_new failed");

    esp_codec_dev_sample_info_t play_info = {
        .sample_rate = CODEC_SAMPLE_RATE, .channel = CODEC_CHANNELS, .bits_per_sample = CODEC_BITS,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_play_dev, &play_info), TAG, "ES8311 open failed");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_play_dev, 70), TAG, "ES8311 set vol failed");
    ESP_LOGI(TAG, "[OK] ES8311 (DAC) ready");

    // ES7210 (ADC)
    audio_codec_i2c_cfg_t i2c_es7210 = {
        .port = CODEC_I2C_PORT, .addr = ES7210_I2C_ADDR, .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *es7210_ctrl = audio_codec_new_i2c_ctrl(&i2c_es7210);
    ESP_RETURN_ON_FALSE(es7210_ctrl, ESP_FAIL, TAG, "ES7210 ctrl interface failed");

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = es7210_ctrl, .master_mode = false,
        .mic_selected = ES7120_SEL_MIC1 | ES7120_SEL_MIC2,
    };
    const audio_codec_if_t *es7210_if = es7210_codec_new(&es7210_cfg);
    ESP_RETURN_ON_FALSE(es7210_if, ESP_FAIL, TAG, "ES7210 codec_new failed");

    esp_codec_dev_cfg_t rec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = es7210_if, .data_if = i2s_data_if,
    };
    s_rec_dev = esp_codec_dev_new(&rec_dev_cfg);
    ESP_RETURN_ON_FALSE(s_rec_dev, ESP_FAIL, TAG, "ES7210 dev_new failed");

    esp_codec_dev_sample_info_t rec_info = {
        .sample_rate = CODEC_SAMPLE_RATE, .channel = CODEC_CHANNELS, .bits_per_sample = CODEC_BITS,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_rec_dev, &rec_info), TAG, "ES7210 open failed");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(s_rec_dev, 42.0f), TAG, "ES7210 set gain failed");
    ESP_LOGI(TAG, "[OK] ES7210 (ADC) ready, gain=42dB");

    s_hw_inited = true;
    return ESP_OK;
}

esp_err_t pipeline_ws_hw_deinit(void)
{
    if (!s_hw_inited) return ESP_OK;
    if (s_play_dev) { esp_codec_dev_close(s_play_dev); esp_codec_dev_delete(s_play_dev); s_play_dev = NULL; }
    if (s_rec_dev)  { esp_codec_dev_close(s_rec_dev);  esp_codec_dev_delete(s_rec_dev);  s_rec_dev  = NULL; }
    if (s_i2s_tx) { i2s_channel_disable(s_i2s_tx); i2s_del_channel(s_i2s_tx); s_i2s_tx = NULL; }
    if (s_i2s_rx) { i2s_channel_disable(s_i2s_rx); i2s_del_channel(s_i2s_rx); s_i2s_rx = NULL; }
    if (s_i2c_owned && s_i2c_bus) { i2c_del_master_bus(s_i2c_bus); s_i2c_owned = false; }
    s_i2c_bus   = NULL;
    s_hw_inited = false;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Recorder
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
    if (!s_rec_open || !s_rec_dev || !s_rec_raw_buf) return -1;
    int out_frames = (int)(size / sizeof(int16_t));
    if (out_frames > REC_RAW_BUF_FRAMES) {
        ESP_LOGE(TAG, "recorder_read: %d frames > max %d", out_frames, REC_RAW_BUF_FRAMES);
        return -1;
    }
    size_t raw_bytes = (size_t)out_frames * 2 * sizeof(int32_t);
    int ret = esp_codec_dev_read(s_rec_dev, s_rec_raw_buf, (int)raw_bytes);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec_dev_read error %d", ret);
        return -1;
    }
    conv_32s_to_16m(s_rec_raw_buf, (int16_t *)buf, out_frames);
    return (int)size;
}

esp_err_t pipeline_ws_recorder_close(void)
{
    s_rec_open = false;
    if (s_rec_raw_buf) { heap_caps_free(s_rec_raw_buf); s_rec_raw_buf = NULL; }
    ESP_LOGI(TAG, "[OK] recorder closed");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Playback task — drains ring buffer (raw PCM), writes to codec
// ---------------------------------------------------------------------------
static void player_task(void *arg)
{
    // 20ms output frame: 320 samples × 2ch × 4bytes = 2560 bytes
    int32_t out_buf[320 * 2];

    while (s_play_task_run) {
        size_t rx_len = 0;
        int16_t *pcm = (int16_t *)xRingbufferReceiveUpTo(
            s_play_rb, &rx_len, pdMS_TO_TICKS(20), 640);
        if (pcm == NULL || rx_len == 0) {
            // Starvation: write silence to keep I2S clock running
            memset(out_buf, 0, sizeof(out_buf));
            esp_codec_dev_write(s_play_dev, out_buf, sizeof(out_buf));
            continue;
        }
        int frames = (int)(rx_len / sizeof(int16_t));
        conv_16m_to_32s(pcm, out_buf, frames);
        vRingbufferReturnItem(s_play_rb, pcm);
        int write_bytes = frames * 2 * (int)sizeof(int32_t);
        if (write_bytes < (int)sizeof(out_buf)) {
            memset((uint8_t *)out_buf + write_bytes, 0, sizeof(out_buf) - write_bytes);
        }
        esp_codec_dev_write(s_play_dev, out_buf, sizeof(out_buf));
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
    ESP_LOGI(TAG, "[OK] player opened (PCM ring buffer)");
    return ESP_OK;
}

esp_err_t pipeline_ws_player_write_pcm(const int16_t *pcm, int frames)
{
    if (!s_play_open || !s_play_rb) return ESP_ERR_INVALID_STATE;
    BaseType_t ok = xRingbufferSend(s_play_rb, pcm,
                                     (size_t)frames * sizeof(int16_t),
                                     pdMS_TO_TICKS(5));
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
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

esp_err_t pipeline_ws_set_volume(int volume)
{
    ESP_RETURN_ON_FALSE(s_play_dev, ESP_ERR_INVALID_STATE, TAG, "Player not initialized");
    return (esp_codec_dev_set_out_vol(s_play_dev, volume) == ESP_CODEC_DEV_OK)
           ? ESP_OK : ESP_FAIL;
}
