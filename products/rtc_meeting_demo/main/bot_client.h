// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0
//
// bot_client — HTTP client for the RTC AI business server.
// Calls /startvoicechat and /stopvoicechat to manage cloud AI agent sessions.

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// All credentials returned by /startvoicechat (matches official volcengine common.h)
typedef struct {
    char room_id[129];
    char uid[129];
    char app_id[25];
    char task_id[129];
    char bot_uid[129];
    char token[257];
} rtc_room_info_t;

/**
 * @brief  Call business server to start a voice chat session.
 *         Blocks until HTTP response received (timeout: 10s).
 *
 * @param[out] info  Filled with room credentials on success.
 * @return ESP_OK on success, ESP_FAIL on HTTP or JSON error.
 */
esp_err_t bot_client_start_chat(rtc_room_info_t *info);

/**
 * @brief  Call business server to stop a voice chat session.
 *
 * @param[in] info  Room info from a previous bot_client_start_chat call.
 * @return ESP_OK on success.
 */
esp_err_t bot_client_stop_chat(const rtc_room_info_t *info);

#ifdef __cplusplus
}
#endif
