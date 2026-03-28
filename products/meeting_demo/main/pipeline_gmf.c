// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0
//
// Audio pipeline for meeting_demo using esp_codec_dev + ESP-IDF I2S driver.
//
// Recording: ES7210 → I2S RX (16kHz 32bit 2ch) → downmix+truncate → 16kHz 16bit mono
// Playback:  8kHz 16bit mono (from volc RTC) → 2× upsample → 16kHz 32bit stereo → ES8311

#include "pipeline_gmf.h"

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

// BSP owns the I2C bus — pipeline borrows it, never creates or deletes it.
// bsp_i2c_get_handle() returns NULL if bsp_i2c_init() hasn't been called yet.
extern i2c_master_bus_handle_t bsp_i2c_get_handle(void);

static const char *TAG = "pipeline_gmf";

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
#define CODEC_I2S_DOUT      BSP_I2S_DOUT   // ESP → ES8311
#define CODEC_I2S_DIN       BSP_I2S_DSIN   // ES7210 → ESP

// ES8311 default I2C address: 0x30 (per driver header ES8311_CODEC_DEFAULT_ADDR)
#define ES8311_I2C_ADDR     (0x30)
// ES7210: use 8-bit addr format as expected by audio_codec_new_i2c_ctrl
// (ES7210_CODEC_DEFAULT_ADDR = 0x80, which is 7-bit 0x40 << 1)
#define ES7210_I2C_ADDR     (0x80)

// Codec runs at 16kHz 32bit stereo (matches speaker product)
#define CODEC_SAMPLE_RATE   (16000)
#define CODEC_BITS          (32)
#define CODEC_CHANNELS      (2)

// volc RTC sends/receives 8kHz 16bit mono
#define VOLC_SAMPLE_RATE    (8000)

// ---------------------------------------------------------------------------
// Playback ring buffer + task config
// Holds ~500ms of 8kHz 16bit mono (320 bytes/frame × 25 frames)
// ---------------------------------------------------------------------------
#define PLAY_RB_SIZE        (320 * 25)
#define PLAY_TASK_STACK     (4 * 1024)
#define PLAY_TASK_PRIORITY  (7)
#define PLAY_TASK_CORE      (0)   // Core 0: separate from audio_feed_task on Core 1

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static i2c_master_bus_handle_t  s_i2c_bus        = NULL;
static i2s_chan_handle_t         s_i2s_tx         = NULL;
static i2s_chan_handle_t         s_i2s_rx         = NULL;
static esp_codec_dev_handle_t   s_play_dev        = NULL;
static esp_codec_dev_handle_t   s_rec_dev         = NULL;
static bool                     s_hw_inited       = false;
static bool                     s_rec_open        = false;
static bool                     s_play_open       = false;
static RingbufHandle_t           s_play_rb         = NULL;
static TaskHandle_t              s_play_task       = NULL;
static volatile bool             s_play_task_run   = false;

// ---------------------------------------------------------------------------
// Sample format helpers
// ---------------------------------------------------------------------------

// 32bit stereo → 16bit mono
// ES7210 on ESP-VoCat outputs MIC data on the RIGHT channel (index 1).
// Mix both channels for better pickup, or use right channel only.
static void conv_32s_to_16m(const int32_t *src, int16_t *dst, int frames)
{
    for (int i = 0; i < frames; i++) {
        // Average left+right for mono mix (avoids dead channel)
        int32_t l = src[i * 2]     >> 16;
        int32_t r = src[i * 2 + 1] >> 16;
        dst[i] = (int16_t)((l + r) / 2);
    }
}

// 8kHz 16bit mono → 16kHz 32bit stereo
// Linear interpolation between consecutive samples avoids ZOH staircase noise.
// prev_sample: last sample from the previous call (for cross-frame smoothing).
static int16_t s_play_prev_sample = 0;

static void expand_8k16m_to_16k32s(const int16_t *src, int32_t *dst, int in_frames)
{
    for (int i = 0; i < in_frames; i++) {
        int16_t s0 = (i == 0) ? s_play_prev_sample : src[i - 1];
        int16_t s1 = src[i];
        // Interpolated mid-point between s0 and s1
        int16_t smid = (int16_t)(((int32_t)s0 + s1) / 2);
        int32_t v0   = (int32_t)smid << 16;   // interpolated frame
        int32_t v1   = (int32_t)s1   << 16;   // original frame
        dst[i * 4]     = v0;   // frame 2i,   L
        dst[i * 4 + 1] = v0;   // frame 2i,   R
        dst[i * 4 + 2] = v1;   // frame 2i+1, L
        dst[i * 4 + 3] = v1;   // frame 2i+1, R
    }
    // Remember last sample for next call
    if (in_frames > 0) {
        s_play_prev_sample = src[in_frames - 1];
    }
}

// ---------------------------------------------------------------------------
// HW init / deinit
// ---------------------------------------------------------------------------

esp_err_t pipeline_gmf_hw_init(void)
{
    if (s_hw_inited) {
        return ESP_OK;
    }

    // ---- I2C: borrow BSP's bus (already initialized by bsp_display_start) ----
    s_i2c_bus = bsp_i2c_get_handle();
    if (!s_i2c_bus) {
        // BSP not yet initialized — init I2C ourselves
        i2c_master_bus_config_t i2c_cfg = {
            .i2c_port          = CODEC_I2C_PORT,
            .sda_io_num        = CODEC_I2C_SDA,
            .scl_io_num        = CODEC_I2C_SCL,
            .clk_source        = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c_bus), TAG, "I2C init failed");
        ESP_LOGI(TAG, "I2C created (SDA=%d SCL=%d)", CODEC_I2C_SDA, CODEC_I2C_SCL);
    } else {
        ESP_LOGI(TAG, "I2C borrowed from BSP");
    }

    // ---- PA power ----
#ifdef BSP_POWER_CODEC_EN
    gpio_set_direction(BSP_POWER_CODEC_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(BSP_POWER_CODEC_EN, 1);
#endif
    gpio_set_direction(BSP_POWER_AMP_IO, GPIO_MODE_OUTPUT);
    gpio_set_level(BSP_POWER_AMP_IO, 1);

    // ---- I2S channel (shared TX+RX on port 0) ----
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
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg), TAG, "I2S TX init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_rx, &std_cfg), TAG, "I2S RX init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "I2S TX enable failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_rx), TAG, "I2S RX enable failed");
    ESP_LOGI(TAG, "I2S ready (port=%d, %dHz %dbit %dch)", CODEC_I2S_PORT,
             CODEC_SAMPLE_RATE, CODEC_BITS, CODEC_CHANNELS);

    // ---- Shared I2S data interface ----
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port       = CODEC_I2S_PORT,
        .tx_handle  = s_i2s_tx,
        .rx_handle  = s_i2s_rx,
    };
    const audio_codec_data_if_t *i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(i2s_data_if, ESP_FAIL, TAG, "I2S data interface failed");

    // ---- GPIO interface (for PA pin control inside codec driver) ----
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    // ---- ES8311 (DAC / playback) ----
    audio_codec_i2c_cfg_t i2c_es8311 = {
        .port       = CODEC_I2C_PORT,
        .addr       = ES8311_I2C_ADDR,
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *es8311_ctrl = audio_codec_new_i2c_ctrl(&i2c_es8311);
    ESP_RETURN_ON_FALSE(es8311_ctrl, ESP_FAIL, TAG, "ES8311 ctrl interface failed");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if    = es8311_ctrl,
        .gpio_if    = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin     = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk    = true,
    };
    const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(es8311_if, ESP_FAIL, TAG, "ES8311 codec_new failed");

    esp_codec_dev_cfg_t play_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_if,
        .data_if  = i2s_data_if,
    };
    s_play_dev = esp_codec_dev_new(&play_dev_cfg);
    ESP_RETURN_ON_FALSE(s_play_dev, ESP_FAIL, TAG, "ES8311 dev_new failed");

    esp_codec_dev_sample_info_t play_info = {
        .sample_rate     = CODEC_SAMPLE_RATE,
        .channel         = CODEC_CHANNELS,
        .bits_per_sample = CODEC_BITS,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_play_dev, &play_info), TAG, "ES8311 open failed");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_play_dev, 70), TAG, "ES8311 set vol failed");
    ESP_LOGI(TAG, "ES8311 (DAC) ready");

    // ---- ES7210 (ADC / recording) ----
    audio_codec_i2c_cfg_t i2c_es7210 = {
        .port       = CODEC_I2C_PORT,
        .addr       = ES7210_I2C_ADDR,
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *es7210_ctrl = audio_codec_new_i2c_ctrl(&i2c_es7210);
    ESP_RETURN_ON_FALSE(es7210_ctrl, ESP_FAIL, TAG, "ES7210 ctrl interface failed");

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if      = es7210_ctrl,
        .master_mode  = false,
        .mic_selected = ES7120_SEL_MIC1 | ES7120_SEL_MIC2,
    };
    const audio_codec_if_t *es7210_if = es7210_codec_new(&es7210_cfg);
    ESP_RETURN_ON_FALSE(es7210_if, ESP_FAIL, TAG, "ES7210 codec_new failed");

    esp_codec_dev_cfg_t rec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210_if,
        .data_if  = i2s_data_if,
    };
    s_rec_dev = esp_codec_dev_new(&rec_dev_cfg);
    ESP_RETURN_ON_FALSE(s_rec_dev, ESP_FAIL, TAG, "ES7210 dev_new failed");

    esp_codec_dev_sample_info_t rec_info = {
        .sample_rate     = CODEC_SAMPLE_RATE,
        .channel         = CODEC_CHANNELS,
        .bits_per_sample = CODEC_BITS,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_rec_dev, &rec_info), TAG, "ES7210 open failed");
    // Set microphone input gain — 0dB default is too low; 30dB matches speaker project
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(s_rec_dev, 30.0f), TAG, "ES7210 set gain failed");
    ESP_LOGI(TAG, "ES7210 (ADC) ready, input gain=30dB");

    s_hw_inited = true;
    return ESP_OK;
}

esp_err_t pipeline_gmf_hw_deinit(void)
{
    if (!s_hw_inited) {
        return ESP_OK;
    }
    if (s_play_dev) {
        esp_codec_dev_close(s_play_dev);
        esp_codec_dev_delete(s_play_dev);
        s_play_dev = NULL;
    }
    if (s_rec_dev) {
        esp_codec_dev_close(s_rec_dev);
        esp_codec_dev_delete(s_rec_dev);
        s_rec_dev = NULL;
    }
    if (s_i2s_tx) {
        i2s_channel_disable(s_i2s_tx);
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
    }
    if (s_i2s_rx) {
        i2s_channel_disable(s_i2s_rx);
        i2s_del_channel(s_i2s_rx);
        s_i2s_rx = NULL;
    }
    // I2C bus is owned by BSP — do NOT delete it
    s_i2c_bus = NULL;
    s_hw_inited = false;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Recorder
// ---------------------------------------------------------------------------

esp_err_t pipeline_gmf_recorder_open(void)
{
    ESP_RETURN_ON_FALSE(s_hw_inited,  ESP_ERR_INVALID_STATE, TAG, "HW not initialized");
    ESP_RETURN_ON_FALSE(!s_rec_open,  ESP_ERR_INVALID_STATE, TAG, "Recorder already open");
    s_rec_open = true;
    ESP_LOGI(TAG, "Recorder opened");
    return ESP_OK;
}

int pipeline_gmf_recorder_read(void *buf, size_t size)
{
    if (!s_rec_open || !s_rec_dev) {
        return -1;
    }

    // out: 16bit mono frames
    int out_frames = (int)(size / sizeof(int16_t));
    // in:  32bit stereo frames (same count)
    size_t raw_bytes = (size_t)out_frames * CODEC_CHANNELS * sizeof(int32_t);

    int32_t *raw = heap_caps_malloc(raw_bytes, MALLOC_CAP_DEFAULT);
    if (!raw) {
        ESP_LOGE(TAG, "recorder_read: alloc %d bytes failed", (int)raw_bytes);
        return -1;
    }

    int ret = esp_codec_dev_read(s_rec_dev, raw, (int)raw_bytes);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec_dev_read error %d", ret);
        heap_caps_free(raw);
        return -1;
    }

    conv_32s_to_16m(raw, (int16_t *)buf, out_frames);
    heap_caps_free(raw);
    return (int)size;
}

esp_err_t pipeline_gmf_recorder_close(void)
{
    s_rec_open = false;
    ESP_LOGI(TAG, "Recorder closed");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Playback task — drains ring buffer, upsample, writes to codec
// Decouples volc callback timing from I2S DMA writes.
// ---------------------------------------------------------------------------

#define PLAY_OUT_BYTES  (320 * CODEC_CHANNELS * (int)sizeof(int32_t))  // 2560 bytes per frame

static void player_task(void *arg)
{
    // out_buf always zeroed at start; partial frames pad with silence automatically
    int32_t *out_buf = heap_caps_calloc(PLAY_OUT_BYTES / sizeof(int32_t),
                                         sizeof(int32_t),
                                         MALLOC_CAP_DMA | MALLOC_CAP_DEFAULT);
    if (!out_buf) {
        ESP_LOGE(TAG, "player_task: out_buf alloc failed");
        vTaskDelete(NULL);
        return;
    }

    while (s_play_task_run) {
        size_t rx_len = 0;
        // NOSPLIT ring buffer: each receive returns exactly one sent item (320 bytes)
        void *item = xRingbufferReceive(s_play_rb, &rx_len, pdMS_TO_TICKS(60));

        if (item == NULL) {
            // Starvation: write silence to keep I2S clock running
            memset(out_buf, 0, PLAY_OUT_BYTES);
            esp_codec_dev_write(s_play_dev, out_buf, PLAY_OUT_BYTES);
            continue;
        }

        int in_frames = (int)(rx_len / sizeof(int16_t));
        expand_8k16m_to_16k32s((const int16_t *)item, out_buf, in_frames);
        vRingbufferReturnItem(s_play_rb, item);

        // Write only actual upsampled bytes; zero remainder to avoid ghost audio
        int actual_bytes = in_frames * CODEC_CHANNELS * (int)sizeof(int32_t) * 2;
        if (actual_bytes < PLAY_OUT_BYTES) {
            memset((uint8_t *)out_buf + actual_bytes, 0, PLAY_OUT_BYTES - actual_bytes);
        }
        esp_codec_dev_write(s_play_dev, out_buf, PLAY_OUT_BYTES);
    }

    heap_caps_free(out_buf);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Player public API
// ---------------------------------------------------------------------------

esp_err_t pipeline_gmf_player_open(void)
{
    ESP_RETURN_ON_FALSE(s_hw_inited,  ESP_ERR_INVALID_STATE, TAG, "HW not initialized");
    ESP_RETURN_ON_FALSE(!s_play_open, ESP_ERR_INVALID_STATE, TAG, "Player already open");

    s_play_prev_sample = 0;

    // NOSPLIT preserves item boundaries: each 320-byte send → exact 320-byte receive
    s_play_rb = xRingbufferCreate(PLAY_RB_SIZE, RINGBUF_TYPE_NOSPLIT);
    ESP_RETURN_ON_FALSE(s_play_rb, ESP_ERR_NO_MEM, TAG, "Play ring buffer alloc failed");

    s_play_task_run = true;
    BaseType_t ret  = xTaskCreatePinnedToCore(
        player_task, "play_task",
        PLAY_TASK_STACK, NULL,
        PLAY_TASK_PRIORITY, &s_play_task,
        PLAY_TASK_CORE);
    if (ret != pdPASS) {
        vRingbufferDelete(s_play_rb);
        s_play_rb = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_play_open = true;
    ESP_LOGI(TAG, "Player opened (ring buffer + task)");
    return ESP_OK;
}

esp_err_t pipeline_gmf_player_write(const void *buf, size_t size)
{
    if (!s_play_open || !s_play_rb) {
        return ESP_ERR_INVALID_STATE;
    }
    // Non-blocking: drop frame if buffer full rather than stalling volc callback
    BaseType_t ok = xRingbufferSend(s_play_rb, buf, size, pdMS_TO_TICKS(5));
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t pipeline_gmf_player_close(void)
{
    if (!s_play_open) return ESP_OK;

    s_play_task_run = false;
    vTaskDelay(pdMS_TO_TICKS(200));   // let task drain and exit
    s_play_task = NULL;

    if (s_play_rb) {
        vRingbufferDelete(s_play_rb);
        s_play_rb = NULL;
    }
    s_play_prev_sample = 0;
    s_play_open = false;
    ESP_LOGI(TAG, "Player closed");
    return ESP_OK;
}

esp_err_t pipeline_gmf_set_volume(int volume)
{
    ESP_RETURN_ON_FALSE(s_play_dev, ESP_ERR_INVALID_STATE, TAG, "Player not initialized");
    return (esp_codec_dev_set_out_vol(s_play_dev, volume) == ESP_CODEC_DEV_OK)
           ? ESP_OK : ESP_FAIL;
}
