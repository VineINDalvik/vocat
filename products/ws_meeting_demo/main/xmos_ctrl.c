// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0
//
// xmos_ctrl.c — XMOS XVF3800 I2C control interface.
// Protocol: [resid(1)] [cmdid(1)] [payload_len(1)] [payload(N)] for writes.
// For reads: write [resid(1)] [cmdid(1)] [0], then read [status(1)] [data(N)].

#include "xmos_ctrl.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "xmos_ctrl";

#define XMOS_I2C_ADDR      0x2C
#define XMOS_I2C_TIMEOUT_MS 100

// XMOS resource IDs
#define XMOS_RES_GPIO      20
#define XMOS_RES_AEC       33
#define XMOS_RES_AUDIO_MGR 35
#define XMOS_RES_APP_SERV  48

// Command IDs
#define CMD_VERSION        0
#define CMD_GPO_WRITE      1
#define CMD_SAVE_CONFIG    3
#define CMD_LED_EFFECT     12
#define CMD_DOA_VALUE      18
#define CMD_VAD_VALUE      19
#define CMD_VOLUME         26
#define CMD_MUTE           27

// GPIO pin for amplifier enable (LOW = enabled)
#define GPIO_AMP_ENABLE    31

static i2c_master_bus_handle_t s_bus = NULL;
static bool s_inited = false;

// I2C write: [resid(1)] [cmdid(1)] [payload_len(1)] [payload(N)]
static esp_err_t xmos_write(uint8_t resid, uint8_t cmdid, const uint8_t *payload, size_t len)
{
    uint8_t buf[16];
    size_t total = 3 + len;
    if (total > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    buf[0] = resid;
    buf[1] = cmdid;
    buf[2] = (uint8_t)len;
    if (len > 0) memcpy(buf + 3, payload, len);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XMOS_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev;
    esp_err_t ret = i2c_master_bus_add_device(s_bus, &dev_cfg, &dev);
    if (ret != ESP_OK) return ret;

    ret = i2c_master_transmit(dev, buf, total, XMOS_I2C_TIMEOUT_MS);
    i2c_master_bus_rm_device(dev);
    return ret;
}

// I2C read: write [resid(1)] [cmdid(1)] [0], delay, then read [status(1)] [data(N)]
static esp_err_t xmos_read(uint8_t resid, uint8_t cmdid, uint8_t *resp, size_t resp_len)
{
    uint8_t cmd[3] = { resid, cmdid, 0 };

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XMOS_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev;
    esp_err_t ret = i2c_master_bus_add_device(s_bus, &dev_cfg, &dev);
    if (ret != ESP_OK) return ret;

    // Send read command
    ret = i2c_master_transmit(dev, cmd, 3, XMOS_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        i2c_master_bus_rm_device(dev);
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(5));

    // Read status byte + data
    uint8_t read_buf[32];
    size_t read_len = 1 + resp_len;
    if (read_len > sizeof(read_buf)) {
        i2c_master_bus_rm_device(dev);
        return ESP_ERR_INVALID_SIZE;
    }

    ret = i2c_master_receive(dev, read_buf, read_len, XMOS_I2C_TIMEOUT_MS);
    if (ret == ESP_OK) {
        // First byte is status (0 = success)
        if (read_buf[0] != 0) {
            ESP_LOGW(TAG, "XMOS read status=%u for resid=%u cmd=%u",
                     read_buf[0], resid, cmdid);
        }
        if (resp && resp_len > 0) {
            memcpy(resp, read_buf + 1, resp_len);
        }
    }
    i2c_master_bus_rm_device(dev);
    return ret;
}

esp_err_t xmos_ctrl_init(i2c_master_bus_handle_t bus)
{
    if (s_inited) return ESP_OK;
    s_bus = bus;

    // Verify XMOS is reachable by reading version
    uint8_t ver[3] = {0};
    esp_err_t ret = xmos_read(XMOS_RES_APP_SERV, CMD_VERSION, ver, sizeof(ver));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "XMOS I2C probe failed (ret=%d), continuing anyway", ret);
    } else {
        ESP_LOGI(TAG, "XMOS XVF3800 detected, version: %u.%u.%u", ver[0], ver[1], ver[2]);
    }

    // Enable amplifier (GPO pin 31, value 0 = enabled)
    uint8_t amp_data[2] = { GPIO_AMP_ENABLE, 0 };
    ret = xmos_write(XMOS_RES_GPIO, CMD_GPO_WRITE, amp_data, 2);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Amplifier enabled (GPO pin %u = LOW)", GPIO_AMP_ENABLE);
    } else {
        ESP_LOGW(TAG, "Amplifier enable failed (ret=%d)", ret);
    }

    // Set volume to maximum
    xmos_ctrl_set_volume(100);

    // Unmute audio output
    xmos_ctrl_set_mute(false);

    s_inited = true;
    return ESP_OK;
}

esp_err_t xmos_ctrl_deinit(void)
{
    s_bus = NULL;
    s_inited = false;
    return ESP_OK;
}

esp_err_t xmos_ctrl_set_volume(int volume)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    uint8_t vol_byte = (uint8_t)(volume & 0xFF);
    esp_err_t ret = xmos_write(XMOS_RES_AUDIO_MGR, CMD_VOLUME, &vol_byte, 1);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "XMOS volume set: %d", volume);
    }
    return ret;
}

esp_err_t xmos_ctrl_set_mute(bool mute)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    uint8_t val = mute ? 1 : 0;
    esp_err_t ret = xmos_write(XMOS_RES_AUDIO_MGR, CMD_MUTE, &val, 1);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "XMOS audio mute: %s", mute ? "ON" : "OFF");
    }
    return ret;
}

bool xmos_ctrl_get_mute(void)
{
    return false;
}

esp_err_t xmos_ctrl_get_doa(int *angle)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    uint8_t resp[2] = {0};
    esp_err_t ret = xmos_read(XMOS_RES_GPIO, CMD_DOA_VALUE, resp, sizeof(resp));
    if (ret == ESP_OK) {
        *angle = (int)((resp[0] << 8) | resp[1]);
    }
    return ret;
}

esp_err_t xmos_ctrl_get_vad(bool *active)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    uint8_t resp[1] = {0};
    esp_err_t ret = xmos_read(XMOS_RES_GPIO, CMD_VAD_VALUE, resp, sizeof(resp));
    if (ret == ESP_OK) {
        *active = (resp[0] != 0);
    }
    return ret;
}