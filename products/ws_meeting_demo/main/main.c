// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "esp_log.h"
#include "nvs_flash.h"
#include "wifi_init.h"
#include "pipeline_ws.h"
#include "ws_session.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

static void auto_start_meeting(void *arg)
{
    ESP_LOGI(TAG, "Waiting for WiFi before starting meeting...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "Auto-starting meeting (listen mode)...");
    ws_session_start_meeting();

    ESP_LOGI(TAG, "Ready. Say \"开始演示样机\" to enter host mode.");

    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ws_meeting_demo starting (ReSpeaker XVF3800)");

    // ---- NVS ----
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ---- Audio hardware (XMOS XVF3800 + I2S) ----
    ESP_ERROR_CHECK(pipeline_ws_hw_init());

    // ---- BOOT button (GPIO9) — toggle host/listen ----
    ws_session_btn_init();

    // ---- WiFi ----
    ret = wifi_init_sta();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi failed");
    } else {
        ESP_LOGI(TAG, "WiFi connected, auto-starting meeting (listen mode)");
        xTaskCreatePinnedToCore(auto_start_meeting, "auto_start", 4096, NULL, 3, NULL, 1);
    }
}