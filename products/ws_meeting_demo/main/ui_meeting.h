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
    UI_STATE_IDLE,          // Initial: "Meeting Assistant" + [Start Meeting] + gear icon
    UI_STATE_MEETING,      // Active:  "Meeting in Progress" + [Host Mode][Stop Meeting]
    UI_STATE_HOST,         // Host:    "Host Mode" + [Exit Host Mode]
    UI_STATE_WIFI_SETTINGS, // WiFi:   keyboard settings panel
} ui_state_t;

/**
 * @brief  Callback type for WiFi credentials saved from UI.
 */
typedef void (*wifi_save_cb_t)(const char *ssid, const char *password);

/**
 * @brief  Register callback invoked when user saves WiFi credentials.
 */
void ui_meeting_register_wifi_save_cb(wifi_save_cb_t cb);

/**
 * @brief  Get current WiFi SSID (from NVS or Kconfig) for pre-filling the form.
 */
void ui_meeting_set_current_wifi(const char *ssid);

/**
 * @brief  Update WiFi settings screen with connection result text.
 *         Safe to call from any task (called via lv_async_call from wifi status cb).
 */
void ui_meeting_set_wifi_result(const char *text);
void ui_meeting_refresh_volume(void);

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
void ui_meeting_show_start_error(const char *text);

/**
 * @brief  Show action/result text below the status label (for function calling).
 *         Auto-clears after 5 seconds. Safe to call from any task.
 *
 * @param text  e.g. "book_meeting_room...", "已预定 A301 14:00-15:00"
 */
void ui_meeting_set_action_text(const char *text);

#ifdef __cplusplus
}
#endif
