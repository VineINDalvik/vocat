// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "esp_log.h"
#include "nvs_flash.h"
#include "bsp/esp_vocat.h"
#include "wifi_init.h"
#include "ui_meeting.h"
#include "pipeline_ws.h"

// bsp_display_backlight_on() exists in esp_vocat.c but is missing from the header
esp_err_t bsp_display_backlight_on(void);

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "ws_meeting_demo starting");

    // ---- NVS ----
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ---- Display ----
    lv_disp_t *disp = bsp_display_start();
    if (!disp) {
        ESP_LOGE(TAG, "Display init failed — halting");
        return;
    }
    bsp_display_lock(0);
    ui_meeting_create();
    bsp_display_unlock();
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "Display ready, free heap: %lu", esp_get_free_heap_size());

    // ---- Audio hardware ----
    ESP_ERROR_CHECK(pipeline_ws_hw_init());

    // ---- WiFi ----
    ret = wifi_init_sta();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi failed — voice features disabled");
    } else {
        ESP_LOGI(TAG, "WiFi connected, ready");
    }

    // LVGL runs its own task; app_main can return.
}
