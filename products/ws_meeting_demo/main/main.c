// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "esp_log.h"
#include "nvs_flash.h"
#include "bsp/esp_vocat.h"
#include "wifi_init.h"
#include "ui_meeting.h"
#include "pipeline_ws.h"
#include "lvgl.h"

// bsp_display_backlight_on() exists in esp_vocat.c but is missing from the header
esp_err_t bsp_display_backlight_on(void);

static const char *TAG = "main";

// WiFi status callback — updates WiFi settings screen via lv_async_call
typedef struct { char text[64]; } wifi_status_msg_t;

static void do_wifi_status_update(void *param)
{
    wifi_status_msg_t *m = (wifi_status_msg_t *)param;
    ui_meeting_set_wifi_result(m->text);
    free(m);
}

static void on_wifi_status(wifi_status_t status)
{
    wifi_status_msg_t *m = malloc(sizeof(wifi_status_msg_t));
    if (!m) return;
    if (status == WIFI_STATUS_CONNECTED) {
        strlcpy(m->text, "Connected!", sizeof(m->text));
        ESP_LOGI(TAG, "WiFi reconnected");
    } else {
        strlcpy(m->text, "Failed - check credentials", sizeof(m->text));
        ESP_LOGE(TAG, "WiFi reconnect failed");
    }
    lv_async_call(do_wifi_status_update, m);
}

// Called when user saves WiFi credentials from the settings screen
static void on_wifi_saved(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "WiFi credentials saved, reconnecting...");
    wifi_reconnect(ssid, password);
}

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

    // Pre-fill current SSID in WiFi settings form
    char cur_ssid[33] = {0};
    char cur_pass[65] = {0};
    if (wifi_load_credentials(cur_ssid, sizeof(cur_ssid), cur_pass, sizeof(cur_pass)) == ESP_OK) {
        ui_meeting_set_current_wifi(cur_ssid);
    } else {
        ui_meeting_set_current_wifi(CONFIG_MEETING_WIFI_SSID);
    }
    ui_meeting_register_wifi_save_cb(on_wifi_saved);

    bsp_display_unlock();
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "Display ready, free heap: %lu", esp_get_free_heap_size());

    // Register WiFi status callback for reconnect feedback
    wifi_register_status_cb(on_wifi_status);

    // ---- Audio hardware ----
    ESP_ERROR_CHECK(pipeline_ws_hw_init());
    ui_meeting_refresh_volume();

    // ---- WiFi ----
    ret = wifi_init_sta();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi failed — tap gear icon to set network");
    } else {
        ESP_LOGI(TAG, "WiFi connected, ready");
    }

    // LVGL runs its own task; app_main can return.
}
