// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_FAILED,
} wifi_status_t;

typedef void (*wifi_status_cb_t)(wifi_status_t status);

/**
 * @brief  Connect to WiFi. Reads NVS credentials first, falls back to Kconfig.
 *         Blocks until connected or timeout (30s).
 */
esp_err_t wifi_init_sta(void);

/**
 * @brief  Save WiFi credentials to NVS.
 */
esp_err_t wifi_save_credentials(const char *ssid, const char *password);

/**
 * @brief  Load WiFi credentials from NVS.
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND or other error if not.
 */
esp_err_t wifi_load_credentials(char *ssid, size_t ssid_len,
                                 char *password, size_t pass_len);

/**
 * @brief  Reconnect to WiFi with new credentials (non-blocking).
 *         Spawns a background task. Register a status callback for results.
 */
esp_err_t wifi_reconnect(const char *ssid, const char *password);

/**
 * @brief  Register callback for WiFi connection status changes.
 */
void wifi_register_status_cb(wifi_status_cb_t cb);

/**
 * @brief  Cancel any ongoing WiFi connection attempt.
 *         Needed before scanning, since scan fails while STA is connecting.
 */
void wifi_stop_connecting(void);

#ifdef __cplusplus
}
#endif
