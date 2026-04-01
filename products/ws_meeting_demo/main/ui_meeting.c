// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "ui_meeting.h"
#include "ws_session.h"
#include "bsp/esp_vocat.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
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

#define MIC_DOT_SIZE  12   // green blinking dot — visible in MEETING state

// ---------------------------------------------------------------------------
// Widget handles
// ---------------------------------------------------------------------------
static lv_obj_t *s_title_label  = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_btn_start    = NULL;   // IDLE:    "Start Meeting"
static lv_obj_t *s_btn_host     = NULL;   // MEETING: "Host Mode"
static lv_obj_t *s_btn_stop     = NULL;   // MEETING: "Stop Meeting"
static lv_obj_t *s_btn_exit     = NULL;   // HOST:    "Exit Host Mode"
static lv_obj_t *s_mic_dot      = NULL;   // MEETING: 12px green blink indicator

static ui_state_t  s_current_state = UI_STATE_IDLE;
static TimerHandle_t s_mic_timer   = NULL;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void btn_start_cb(lv_event_t *e);
static void btn_host_cb(lv_event_t *e);
static void btn_stop_cb(lv_event_t *e);
static void btn_exit_cb(lv_event_t *e);

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
// Mic dot blink timer callback (runs in FreeRTOS timer task — use lv_async_call)
// ---------------------------------------------------------------------------
static void mic_dot_toggle(void *param)
{
    if (!s_mic_dot) return;
    if (lv_obj_has_flag(s_mic_dot, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(s_mic_dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_mic_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

static void mic_timer_cb(TimerHandle_t xTimer)
{
    if (s_current_state == UI_STATE_MEETING) {
        lv_async_call(mic_dot_toggle, NULL);
    }
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

    // ---- Mic dot (green, 12px circle, MEETING state blink indicator) ----
    s_mic_dot = lv_obj_create(screen);
    lv_obj_set_size(s_mic_dot, MIC_DOT_SIZE, MIC_DOT_SIZE);
    lv_obj_set_style_radius(s_mic_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_mic_dot, lv_color_make(0x00, 0xCC, 0x44), 0);
    lv_obj_set_style_border_width(s_mic_dot, 0, 0);
    lv_obj_align(s_mic_dot, LV_ALIGN_TOP_MID, 0, TITLE_Y - MIC_DOT_SIZE - 8);
    lv_obj_add_flag(s_mic_dot, LV_OBJ_FLAG_HIDDEN);

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

    // ---- Mic blink timer (500ms, auto-reload) ----
    s_mic_timer = xTimerCreate("mic_blink", pdMS_TO_TICKS(500), pdTRUE, NULL, mic_timer_cb);
    if (s_mic_timer) xTimerStart(s_mic_timer, 0);

    // Start in IDLE
    ui_meeting_set_state(UI_STATE_IDLE);
}

// ---------------------------------------------------------------------------
// apply_state — call with LVGL lock held
// ---------------------------------------------------------------------------
static void apply_state(ui_state_t state)
{
    lv_obj_add_flag(s_btn_start,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_host,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_stop,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_exit,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_mic_dot,      LV_OBJ_FLAG_HIDDEN);

    switch (state) {
    case UI_STATE_IDLE:
        lv_label_set_text(s_title_label, "Meeting Assistant");
        lv_obj_clear_flag(s_btn_start, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "state → IDLE, mic dot hidden");
        break;

    case UI_STATE_MEETING:
        lv_label_set_text(s_title_label, "Meeting in Progress");
        lv_obj_clear_flag(s_btn_host,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_btn_stop,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
        // mic dot starts visible; timer will blink it
        lv_obj_clear_flag(s_mic_dot, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "state → MEETING, mic dot visible (blink 500ms)");
        break;

    case UI_STATE_HOST:
        lv_label_set_text(s_title_label, "Host Mode");
        lv_obj_clear_flag(s_btn_exit,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
        // mic dot hidden in HOST (muted during TTS playback)
        ESP_LOGI(TAG, "state → HOST, mic dot hidden");
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
// Button callbacks  (run inside LVGL task — apply_state() is safe here,
//                   ws_session_* are non-blocking queue sends)
// ---------------------------------------------------------------------------

static void btn_start_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "[btn] Start Meeting");
    apply_state(UI_STATE_MEETING);
    lv_label_set_text(s_status_label, "Connecting...");
    ws_session_start_meeting();
}

static void btn_host_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "[btn] Host Mode");
    apply_state(UI_STATE_HOST);
    lv_label_set_text(s_status_label, "Connecting...");
    ws_session_enter_host();
}

static void btn_stop_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "[btn] Stop Meeting");
    apply_state(UI_STATE_IDLE);
    ws_session_stop_meeting();
}

static void btn_exit_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "[btn] Exit Host Mode");
    apply_state(UI_STATE_MEETING);
    lv_label_set_text(s_status_label, "Reconnecting...");
    ws_session_exit_host();
}
