// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Connect to WiFi using Kconfig SSID/password.
 *         Blocks until connected or timeout (30s).
 * @return ESP_OK on success, ESP_FAIL on timeout.
 */
esp_err_t wifi_init_sta(void);

#ifdef __cplusplus
}
#endif
