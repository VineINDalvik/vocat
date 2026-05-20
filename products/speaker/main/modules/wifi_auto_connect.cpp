/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Pre-seed WiFi AP credentials into Brookesia WiFi Service NVS namespace.
 * The framework's try_load_data() on startup reads "LastAp" and connects.
 * We only write if LastAp doesn't already exist (won't overwrite user config).
 * We do NOT call esp_wifi_connect() ourselves — that conflicts with the
 * Brookesia framework's own WiFi initialization in speaker->begin().
 */
#include "wifi_auto_connect.h"
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_auto";

// Brookesia WiFi Service uses its service name as NVS namespace
static const char *NVS_NS = "Wifi";
static const char *NVS_KEY = "LastAp";

esp_err_t wifi_auto_connect(void)
{
    struct ApProfile { const char *ssid; const char *password; };
    static const ApProfile profiles[] = {
        {CONFIG_SPEAKER_WIFI_AP1_SSID, CONFIG_SPEAKER_WIFI_AP1_PASSWORD},
        {CONFIG_SPEAKER_WIFI_AP2_SSID, CONFIG_SPEAKER_WIFI_AP2_PASSWORD},
        {CONFIG_SPEAKER_WIFI_AP3_SSID, CONFIG_SPEAKER_WIFI_AP3_PASSWORD},
    };
    static const int num_profiles = sizeof(profiles) / sizeof(profiles[0]);

    // Find first configured AP
    const ApProfile *target = nullptr;
    for (int i = 0; i < num_profiles; i++) {
        if (strlen(profiles[i].ssid) > 0) {
            target = &profiles[i];
            break;
        }
    }
    if (!target) {
        ESP_LOGI(TAG, "No WiFi profiles configured, skipping");
        return ESP_OK;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open '%s' failed (0x%x), trying init", NVS_NS, err);
        nvs_flash_init();
        err = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open NVS namespace '%s'", NVS_NS);
        return ESP_FAIL;
    }

    // Check if user already has a LastAp configured via Settings UI
    char existing[256] = {};
    size_t existing_len = sizeof(existing);
    bool has_existing = (nvs_get_str(handle, NVS_KEY, existing, &existing_len) == ESP_OK && existing_len > 2);
    if (has_existing) {
        ESP_LOGI(TAG, "LastAp already in NVS, not overwriting");
        nvs_close(handle);
        return ESP_OK;
    }

    // Write as JSON: {"ssid":"xxx","password":"yyy","is_connectable":true}
    char json[320];
    snprintf(json, sizeof(json),
             "{\"ssid\":\"%s\",\"password\":\"%s\",\"is_connectable\":true}",
             target->ssid, target->password);

    err = nvs_set_str(handle, NVS_KEY, json);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write LastAp to NVS");
        nvs_close(handle);
        return ESP_FAIL;
    }

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Pre-seeded WiFi AP \"%s\" in NVS", target->ssid);
    }
    return err;
}
