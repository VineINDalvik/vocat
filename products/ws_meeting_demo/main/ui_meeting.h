// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0
//
// ui_meeting — 360×360 round-screen LVGL UI for ws_meeting_demo.
//
// Three states driven by button callbacks; state changes call into
// ws_session_* (non-blocking queue sends) and update status text via
// ws_session_update_ui_status().

#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_STATE_IDLE,     // Initial: "Meeting Assistant" + [Start Meeting]
    UI_STATE_MEETING,  // Active:  "Meeting in Progress" + [Host Mode][Stop Meeting]
    UI_STATE_HOST,     // Host:    "Host Mode - RTC AI" + [Exit Host Mode]
} ui_state_t;

/**
 * @brief  Create all LVGL widgets. Call once after bsp_display_start().
 *         Must be called with LVGL lock held.
 */
void ui_meeting_create(void);

/**
 * @brief  Transition to a new UI state.
 *         Safe to call from any task (acquires bsp_display_lock internally).
 */
void ui_meeting_set_state(ui_state_t state);

/**
 * @brief  Update the bottom status text (MEETING / HOST states only).
 *         Safe to call from any task.
 *
 * @param text  e.g. "● Connected", "● Connecting...", "● Connection Error"
 */
void ui_meeting_set_status(const char *text);

#ifdef __cplusplus
}
#endif
