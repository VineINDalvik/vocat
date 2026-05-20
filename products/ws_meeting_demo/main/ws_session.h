// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WS_SESSION_IDLE,
    WS_SESSION_CONNECTING,
    WS_SESSION_MEETING,
    WS_SESSION_HOST,
    WS_SESSION_ERROR,
} ws_session_state_t;

// Shared mute flag: set true by mp3_player during playback, read by host_ws feed_task
extern volatile bool g_mic_muted;

// Button callbacks call these (fast, non-blocking — enqueues to cmd_task)
esp_err_t ws_session_start_meeting(void);
esp_err_t ws_session_stop_meeting(void);
esp_err_t ws_session_enter_host(void);
esp_err_t ws_session_exit_host(void);

// Thread-safe UI text update (lv_async_call internally)
void ws_session_update_ui_status(const char *text);
void ws_session_update_ui_action(const char *text);

ws_session_state_t ws_session_get_state(void);

#ifdef __cplusplus
}
#endif
