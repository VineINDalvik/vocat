// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0
//
// volc_rtc_session — thin wrapper around volc_conv_ai SDK for meeting_demo.
//
// Manages the full lifecycle: create → start → [audio loop] → stop → destroy.
// Audio I/O is wired to pipeline_gmf internally; callers need only start/stop.

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOLC_SESSION_IDLE,        // Not started
    VOLC_SESSION_CONNECTING,  // volc_start called, waiting for VOLC_EV_CONNECTED
    VOLC_SESSION_CONNECTED,   // Ready for voice Q&A
    VOLC_SESSION_ERROR,       // Connection or runtime error
} volc_session_state_t;

/**
 * @brief  State-change callback, fired from an internal FreeRTOS task.
 *         Must be safe to call without holding any locks.
 */
typedef void (*volc_session_state_cb_t)(volc_session_state_t state, void *ctx);

/**
 * @brief  Start a Volcano Engine RTC session.
 *
 * Initialises audio pipeline (pipeline_gmf_hw_init), creates and starts
 * the volc engine, then launches the audio feed task. Returns immediately;
 * connection result arrives via @p cb.
 *
 * @param bot_id   Volcano Engine Bot ID (from Kconfig / caller).
 * @param cb       State-change callback (may be NULL).
 * @param ctx      Passed verbatim to @p cb.
 * @return         ESP_OK if start sequence kicked off, error otherwise.
 */
esp_err_t volc_rtc_session_start(const char *bot_id,
                                  volc_session_state_cb_t cb,
                                  void *ctx);

/**
 * @brief  Stop the current session and release all resources.
 *         Safe to call even if not started (returns ESP_OK).
 */
esp_err_t volc_rtc_session_stop(void);

/**
 * @brief  Query the current session state (thread-safe).
 */
volc_session_state_t volc_rtc_session_get_state(void);

#ifdef __cplusplus
}
#endif
