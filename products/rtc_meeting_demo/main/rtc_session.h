// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0
//
// rtc_session — RTC session manager using VolcEngineRTCLite SDK.
// Replaces volc_rtc_session (which used volc_conv_ai SDK).

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RTC_SESSION_IDLE,        // Not started
    RTC_SESSION_CONNECTING,  // HTTP + RTC join in progress
    RTC_SESSION_CONNECTED,   // In room, audio flowing
    RTC_SESSION_ERROR,       // Connection or runtime error
} rtc_session_state_t;

typedef void (*rtc_session_state_cb_t)(rtc_session_state_t state, void *ctx);

/**
 * @brief  Start an RTC voice chat session.
 *
 * Flow: HTTP /startvoicechat → credentials → byte_rtc_create/init/join → audio loop
 *
 * @param cb   State-change callback (may be NULL).
 * @param ctx  Passed to cb.
 * @return ESP_OK if start sequence kicked off.
 */
esp_err_t rtc_session_start(rtc_session_state_cb_t cb, void *ctx);

/**
 * @brief  Stop the current session and release all resources.
 */
esp_err_t rtc_session_stop(void);

/**
 * @brief  Query current session state (thread-safe).
 */
rtc_session_state_t rtc_session_get_state(void);

#ifdef __cplusplus
}
#endif
