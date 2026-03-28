#include "esp_log.h"
#include "nvs_flash.h"
#include "bsp/esp_vocat.h"
#include "volc_conv_ai.h"
#include "wifi_init.h"
#include "ui_meeting.h"

// bsp_display_backlight_on() exists in esp_vocat.c but is missing from the header
esp_err_t bsp_display_backlight_on(void);

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Meeting Demo starting (volc_conv_ai SDK v%s)",
             volc_get_version());

    // ---- NVS ----
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ---- Display ----
    // bsp_display_start() internally calls bsp_display_brightness_init().
    // We must NOT call it again — just set brightness after start.
    lv_disp_t *disp = bsp_display_start();
    if (!disp) {
        ESP_LOGE(TAG, "Display init failed — halting");
        return;
    }
    // ---- Build UI before turning on backlight (prevents white flash) ----
    bsp_display_lock(0);
    ui_meeting_create();
    bsp_display_unlock();

    // Turn on backlight only after UI is ready
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "Display ready, free heap: %lu", esp_get_free_heap_size());

    // ---- WiFi ----
    ret = wifi_init_sta();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi failed — voice features disabled");
        // UI stays in IDLE, user can see the device booted
    } else {
        ESP_LOGI(TAG, "WiFi connected, ready");
    }

    // LVGL runs its own task; app_main can return.
}
