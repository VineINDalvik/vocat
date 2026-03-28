// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "ui_meeting.h"
#include "volc_rtc_session.h"
#include "bsp/esp_vocat.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ui_meeting";

// ---------------------------------------------------------------------------
// Layout constants (360×360 round screen)
// ---------------------------------------------------------------------------
#define SCREEN_W    360
#define SCREEN_H    360
#define TITLE_Y     55
#define BTN_Y       155
#define BTN_H       54
#define BTN_W_WIDE  200
#define BTN_W_HALF  130
#define BTN_GAP     10
#define STATUS_Y    285

// ---------------------------------------------------------------------------
// Widget handles
// ---------------------------------------------------------------------------
static lv_obj_t *s_title_label  = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_btn_start    = NULL;   // IDLE:    "Start Meeting"
static lv_obj_t *s_btn_host     = NULL;   // MEETING: "Host Mode"
static lv_obj_t *s_btn_stop     = NULL;   // MEETING: "Stop Meeting"
static lv_obj_t *s_btn_exit     = NULL;   // HOST:    "Exit Host Mode"

static ui_state_t s_current_state = UI_STATE_IDLE;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void btn_start_cb(lv_event_t *e);
static void btn_host_cb(lv_event_t *e);
static void btn_stop_cb(lv_event_t *e);
static void btn_exit_cb(lv_event_t *e);
static void session_state_cb(volc_session_state_t state, void *ctx);

// ---------------------------------------------------------------------------
// Button factory
// ---------------------------------------------------------------------------
static lv_obj_t *make_button(lv_obj_t *parent, const char *label_text,
                              int x, int y, int w, int h,
                              lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 10, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label_text);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

// ---------------------------------------------------------------------------
// ui_meeting_create
// ---------------------------------------------------------------------------
void ui_meeting_create(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    // ---- Title label ----
    s_title_label = lv_label_create(screen);
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_title_label, lv_color_white(), 0);
    lv_obj_set_width(s_title_label, SCREEN_W - 40);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_title_label, "Meeting Assistant");
    lv_obj_set_style_text_align(s_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, TITLE_Y);

    // ---- Status label (bottom, hidden in IDLE) ----
    s_status_label = lv_label_create(screen);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_set_width(s_status_label, SCREEN_W - 40);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_status_label, "");
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, STATUS_Y);
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);

    // ---- IDLE: [Start Meeting] ----
    s_btn_start = make_button(screen, "Start Meeting",
                              (SCREEN_W - BTN_W_WIDE) / 2, BTN_Y,
                              BTN_W_WIDE, BTN_H,
                              btn_start_cb);

    // ---- MEETING: [Host Mode] [Stop Meeting] ----
    int two_btn_total = BTN_W_HALF * 2 + BTN_GAP;
    int two_btn_x     = (SCREEN_W - two_btn_total) / 2;

    s_btn_host = make_button(screen, "Host Mode",
                             two_btn_x, BTN_Y,
                             BTN_W_HALF, BTN_H,
                             btn_host_cb);

    s_btn_stop = make_button(screen, "Stop Meeting",
                             two_btn_x + BTN_W_HALF + BTN_GAP, BTN_Y,
                             BTN_W_HALF, BTN_H,
                             btn_stop_cb);

    // ---- HOST: [Exit Host Mode] ----
    s_btn_exit = make_button(screen, "Exit Host Mode",
                             (SCREEN_W - BTN_W_WIDE) / 2, BTN_Y,
                             BTN_W_WIDE, BTN_H,
                             btn_exit_cb);

    // Start in IDLE
    ui_meeting_set_state(UI_STATE_IDLE);
}

// ---------------------------------------------------------------------------
// ui_meeting_set_state (internal layout logic, call with lock held)
// ---------------------------------------------------------------------------
static void apply_state(ui_state_t state)
{
    // Hide all buttons and status first
    lv_obj_add_flag(s_btn_start,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_host,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_stop,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_exit,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);

    switch (state) {
    case UI_STATE_IDLE:
        lv_label_set_text(s_title_label, "Meeting Assistant");
        lv_obj_clear_flag(s_btn_start, LV_OBJ_FLAG_HIDDEN);
        break;

    case UI_STATE_MEETING:
        lv_label_set_text(s_title_label, "Meeting in Progress");
        lv_obj_clear_flag(s_btn_host,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_btn_stop,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
        break;

    case UI_STATE_HOST:
        lv_label_set_text(s_title_label, "Host Mode - Clary");
        lv_obj_clear_flag(s_btn_exit,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
        break;
    }

    s_current_state = state;
}

void ui_meeting_set_state(ui_state_t state)
{
    if (bsp_display_lock(100)) {
        apply_state(state);
        bsp_display_unlock();
    }
}

// ---------------------------------------------------------------------------
// ui_meeting_set_status
// ---------------------------------------------------------------------------
void ui_meeting_set_status(const char *text)
{
    if (!text) return;
    if (bsp_display_lock(100)) {
        lv_label_set_text(s_status_label, text);
        bsp_display_unlock();
    }
}

// ---------------------------------------------------------------------------
// Background helpers — volc ops must NOT run in the LVGL callback context
// ---------------------------------------------------------------------------

static void task_session_start(void *arg)
{
    volc_rtc_session_start(CONFIG_MEETING_VOLC_BOT_ID, session_state_cb, NULL);
    vTaskDelete(NULL);
}

static void task_session_stop(void *arg)
{
    volc_rtc_session_stop();
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Button callbacks  (run inside LVGL task — must be fast and non-blocking)
//
// Rules:
//  • Update LVGL widgets directly — do NOT call bsp_display_lock() here,
//    the LVGL task already holds it.
//  • Dispatch any blocking work (I2C init, network) to a separate task.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Button callbacks
// Rules:
//  • Run in LVGL task — call apply_state() directly, NO bsp_display_lock().
//  • Blocking work (I2C, network, volc) dispatched to background task.
//  • Meeting mode (HTTP/WS) is MOCKED — only Host Mode uses real volc RTC.
// ---------------------------------------------------------------------------

static void btn_start_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "[btn] Start Meeting (mock mode)");
    apply_state(UI_STATE_MEETING);
    // Meeting mode is mocked: show connected immediately without real HTTP/WS
    lv_label_set_text(s_status_label, "* Meeting (Mock)");
}

static void btn_host_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "[btn] Host Mode — starting volc RTC");
    apply_state(UI_STATE_HOST);
    lv_label_set_text(s_status_label, "* Connecting...");
    // Real volc RTC session — dispatched to background task
    xTaskCreate(task_session_start, "sess_start", 6 * 1024, NULL, 5, NULL);
}

static void btn_stop_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "[btn] Stop Meeting (mock mode)");
    // Meeting mode was mocked — nothing to tear down
    apply_state(UI_STATE_IDLE);
}

static void btn_exit_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "[btn] Exit Host Mode — stopping volc RTC");
    // Show MEETING state immediately; session stops in background
    apply_state(UI_STATE_MEETING);
    lv_label_set_text(s_status_label, "* Meeting (Mock)");
    xTaskCreate(task_session_stop, "sess_stop", 4 * 1024, NULL, 5, NULL);
}

// ---------------------------------------------------------------------------
// Session state callback — fired from volc_rtc_session internal task
// ---------------------------------------------------------------------------
static void session_state_cb(volc_session_state_t state, void *ctx)
{
    const char *text;
    lv_color_t  color;

    switch (state) {
    case VOLC_SESSION_IDLE:
        text  = "* Idle";
        color = lv_color_make(0x80, 0x80, 0x80);
        break;
    case VOLC_SESSION_CONNECTING:
        text  = "* Connecting...";
        color = lv_color_make(0xFF, 0xCC, 0x00);
        break;
    case VOLC_SESSION_CONNECTED:
        text  = "*Connected";
        color = lv_color_make(0x00, 0xCC, 0x44);
        break;
    case VOLC_SESSION_ERROR:
        text  = "*Connection Error";
        color = lv_color_make(0xFF, 0x44, 0x44);
        break;
    default:
        return;
    }

    if (bsp_display_lock(100)) {
        lv_label_set_text(s_status_label, text);
        lv_obj_set_style_text_color(s_status_label, color, 0);
        bsp_display_unlock();
    }
}
