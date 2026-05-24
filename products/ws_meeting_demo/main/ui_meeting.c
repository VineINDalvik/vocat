// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "ui_meeting.h"
#include "wifi_init.h"
#include "ws_session.h"
#include "bsp/esp_vocat.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ui_meeting";

#define SCREEN_W    360
#define SCREEN_H    360
#define TITLE_Y     55
#define BTN_Y       155
#define BTN_H       54
#define BTN_W_WIDE  200
#define BTN_W_HALF  130
#define BTN_GAP     10
#define STATUS_Y    270
#define ACTION_Y    295
#define MIC_DOT_SIZE  12

// ---------------------------------------------------------------------------
// Widget handles
// ---------------------------------------------------------------------------
static lv_obj_t *s_title_label  = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_action_label = NULL;
static lv_obj_t *s_btn_start    = NULL;
static lv_obj_t *s_btn_host     = NULL;
static lv_obj_t *s_btn_stop     = NULL;
static lv_obj_t *s_btn_exit     = NULL;
static lv_obj_t *s_btn_interrupt = NULL;
static lv_obj_t *s_btn_wifi    = NULL;
static lv_obj_t *s_mic_dot      = NULL;

// WiFi settings
static lv_obj_t *s_wifi_ap_list = NULL;     // lv_list for scan results
static lv_obj_t *s_wifi_ta_pass = NULL;    // password input
static lv_obj_t *s_wifi_kb      = NULL;     // keyboard
static lv_obj_t *s_wifi_btn_save = NULL;
static lv_obj_t *s_wifi_btn_back = NULL;
static lv_obj_t *s_wifi_status   = NULL;
static lv_obj_t *s_wifi_ssid_label = NULL;
static lv_obj_t *s_wifi_btn_show = NULL;  // toggle password visibility // shows selected SSID above password

static char s_selected_ssid[33] = {0};  // SSID from scan list tap
static wifi_save_cb_t s_wifi_save_cb = NULL;
static char s_current_ssid[33] = {0};

static ui_state_t  s_current_state = UI_STATE_IDLE;
static TimerHandle_t s_mic_timer   = NULL;
static esp_timer_handle_t s_action_clear_timer = NULL;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void btn_start_cb(lv_event_t *e);
static void btn_host_cb(lv_event_t *e);
static void btn_stop_cb(lv_event_t *e);
static void btn_exit_cb(lv_event_t *e);
static void btn_interrupt_cb(lv_event_t *e);
static void btn_wifi_cb(lv_event_t *e);
static void wifi_save_cb_internal(lv_event_t *e);
static void wifi_back_cb_internal(lv_event_t *e);
static void wifi_ta_focus_cb(lv_event_t *e);
static void wifi_ap_selected_cb(lv_event_t *e);
static void wifi_show_pass_cb(lv_event_t *e);
static void wifi_do_scan(void);
static void show_wifi_scan_results(void *arg);

void ui_meeting_register_wifi_save_cb(wifi_save_cb_t cb)
{
    s_wifi_save_cb = cb;
}

void ui_meeting_set_current_wifi(const char *ssid)
{
    strlcpy(s_current_ssid, ssid ? ssid : "", sizeof(s_current_ssid));
}

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
// Mic dot blink
// ---------------------------------------------------------------------------
static void mic_dot_toggle(void *param)
{
    if (!s_mic_dot) return;
    if (lv_obj_has_flag(s_mic_dot, LV_OBJ_FLAG_HIDDEN))
        lv_obj_clear_flag(s_mic_dot, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_mic_dot, LV_OBJ_FLAG_HIDDEN);
}

static void mic_timer_cb(TimerHandle_t xTimer)
{
    if (s_current_state == UI_STATE_MEETING) lv_async_call(mic_dot_toggle, NULL);
}

// ---------------------------------------------------------------------------
// Action text auto-clear
// ---------------------------------------------------------------------------
static uint32_t s_action_generation = 0;

static void do_action_clear(void *param)
{
    uint32_t gen = (uint32_t)(uintptr_t)param;
    if (gen != s_action_generation) return;
    if (s_action_label) {
        lv_label_set_text(s_action_label, "");
        lv_obj_add_flag(s_action_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void action_clear_timer_cb(void *arg)
{
    lv_async_call(do_action_clear, (void *)(uintptr_t)s_action_generation);
}

// ---------------------------------------------------------------------------
// WiFi scan: runs in a background task, posts results via lv_async_call
// ---------------------------------------------------------------------------
#define MAX_AP 10

typedef struct {
    int count;
    char ssids[MAX_AP][33];
    int8_t rssi[MAX_AP];
    bool open[MAX_AP];  // true = no password needed
} wifi_scan_result_t;

static void scan_task(void *arg)
{
    // Cancel any ongoing connection attempt — scan requires idle STA
    wifi_stop_connecting();
    vTaskDelay(pdMS_TO_TICKS(300));

    wifi_scan_result_t *result = calloc(1, sizeof(wifi_scan_result_t));
    if (!result) { vTaskDelete(NULL); return; }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi scan failed: %s", esp_err_to_name(err));
        free(result);
        vTaskDelete(NULL);
        return;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num > MAX_AP) ap_num = MAX_AP;

    wifi_ap_record_t ap_records[MAX_AP];
    err = esp_wifi_scan_get_ap_records(&ap_num, ap_records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_ap_records failed: %s", esp_err_to_name(err));
        free(result);
        vTaskDelete(NULL);
        return;
    }

    result->count = ap_num;
    for (int i = 0; i < ap_num; i++) {
        strlcpy(result->ssids[i], (char *)ap_records[i].ssid, 33);
        result->rssi[i] = ap_records[i].rssi;
        result->open[i] = (ap_records[i].authmode == WIFI_AUTH_OPEN);
        ESP_LOGI(TAG, "  AP[%d]: \"%s\" rssi=%d auth=%d",
                 i, result->ssids[i], result->rssi[i], ap_records[i].authmode);
    }

    ESP_LOGI(TAG, "scan found %d APs", ap_num);
    lv_async_call(show_wifi_scan_results, result);
    vTaskDelete(NULL);
}

static void wifi_do_scan(void)
{
    lv_label_set_text(s_wifi_status, "Scanning...");
    lv_obj_clear_flag(s_wifi_status, LV_OBJ_FLAG_HIDDEN);

    xTaskCreatePinnedToCoreWithCaps(
        scan_task, "wifi_scan", 5 * 1024, NULL, 4,
        NULL, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// Called on LVGL task via lv_async_call after scan completes
static void show_wifi_scan_results(void *arg)
{
    wifi_scan_result_t *result = (wifi_scan_result_t *)arg;
    if (!result || s_current_state != UI_STATE_WIFI_SETTINGS) {
        free(result);
        return;
    }

    // Clear previous list items
    lv_obj_clean(s_wifi_ap_list);

    if (result->count == 0) {
        lv_label_set_text(s_wifi_status, "No networks found");
    } else {
        lv_label_set_text(s_wifi_status, "");
        lv_obj_add_flag(s_wifi_status, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < result->count; i++) {
        char buf[48];
        int rssi_bar = (-result->rssi[i]) / 10;  // rough: 20=strong, 9=weak
        if (rssi_bar > 4) rssi_bar = 4;
        char bars[6];
        memset(bars, '|', rssi_bar);
        bars[rssi_bar] = '\0';

        snprintf(buf, sizeof(buf), "%s %s%s",
                 result->ssids[i],
                 result->open[i] ? "[Open] " : "",
                 bars);

        lv_obj_t *btn = lv_list_add_btn(s_wifi_ap_list, NULL, buf);
        // Store SSID index as user data
        char *ssid_copy = strdup(result->ssids[i]);
        lv_obj_set_user_data(btn, ssid_copy);
        lv_obj_add_event_cb(btn, wifi_ap_selected_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_set_style_text_font(lv_obj_get_child(btn, 0), &lv_font_montserrat_14, 0);
    }

    free(result);
}

// User tapped an AP in the scan list
static void wifi_ap_selected_cb(lv_event_t *e)
{
    // Use current_target — the button — not the original target (could be label child)
    lv_obj_t *btn = lv_event_get_current_target(e);
    char *ssid = (char *)lv_obj_get_user_data(btn);
    if (!ssid) {
        ESP_LOGW(TAG, "AP tap: user_data is NULL");
        return;
    }

    strlcpy(s_selected_ssid, ssid, sizeof(s_selected_ssid));
    ESP_LOGI(TAG, "selected SSID: \"%s\"", s_selected_ssid);

    // Show password entry (hide AP list, show password UI)
    lv_obj_add_flag(s_wifi_ap_list, LV_OBJ_FLAG_HIDDEN);

    char label[48];
    snprintf(label, sizeof(label), "SSID: %s", s_selected_ssid);
    lv_label_set_text(s_wifi_ssid_label, label);
    lv_obj_clear_flag(s_wifi_ssid_label, LV_OBJ_FLAG_HIDDEN);

    // Pre-fill saved password if this SSID was previously saved
    char saved_pass[65] = {0};
    if (wifi_load_credentials(s_selected_ssid, sizeof(s_selected_ssid), saved_pass, sizeof(saved_pass)) == ESP_OK) {
        lv_textarea_set_text(s_wifi_ta_pass, saved_pass);
        ESP_LOGI(TAG, "pre-filled saved password for \"%s\"", s_selected_ssid);
    } else {
        lv_textarea_set_text(s_wifi_ta_pass, "");
    }
    lv_obj_clear_flag(s_wifi_ta_pass, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_wifi_btn_show, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_wifi_btn_save, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_wifi_btn_back, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_wifi_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_wifi_kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_wifi_kb, s_wifi_ta_pass);
}

// ---------------------------------------------------------------------------
// ui_meeting_create
// ---------------------------------------------------------------------------
void ui_meeting_create(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    // ---- Title ----
    s_title_label = lv_label_create(screen);
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_title_label, lv_color_white(), 0);
    lv_obj_set_width(s_title_label, SCREEN_W - 40);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_title_label, "Meeting Assistant");
    lv_obj_set_style_text_align(s_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, TITLE_Y);

    // ---- Status label ----
    s_status_label = lv_label_create(screen);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_set_width(s_status_label, SCREEN_W - 40);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_status_label, "");
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, STATUS_Y);
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);

    // ---- Action label ----
    s_action_label = lv_label_create(screen);
    lv_obj_set_style_text_font(s_action_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_action_label, lv_color_make(0x60, 0xE0, 0x80), 0);
    lv_obj_set_width(s_action_label, SCREEN_W - 40);
    lv_label_set_long_mode(s_action_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_action_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_action_label, "");
    lv_obj_align(s_action_label, LV_ALIGN_TOP_MID, 0, ACTION_Y);
    lv_obj_add_flag(s_action_label, LV_OBJ_FLAG_HIDDEN);

    // ---- Mic dot ----
    s_mic_dot = lv_obj_create(screen);
    lv_obj_set_size(s_mic_dot, MIC_DOT_SIZE, MIC_DOT_SIZE);
    lv_obj_set_style_radius(s_mic_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_mic_dot, lv_color_make(0x00, 0xCC, 0x44), 0);
    lv_obj_set_style_border_width(s_mic_dot, 0, 0);
    lv_obj_align(s_mic_dot, LV_ALIGN_TOP_MID, 0, TITLE_Y - MIC_DOT_SIZE - 8);
    lv_obj_add_flag(s_mic_dot, LV_OBJ_FLAG_HIDDEN);

    // ---- IDLE: [Start Meeting] + [WiFi] ----
    s_btn_start = make_button(screen, "Start Meeting",
                              (SCREEN_W - BTN_W_WIDE) / 2, BTN_Y,
                              BTN_W_WIDE, BTN_H, btn_start_cb);

    s_btn_wifi = make_button(screen, "WiFi",
                              (SCREEN_W - 120) / 2, BTN_Y + BTN_H + 10,
                              120, 42, btn_wifi_cb);

    // ---- MEETING: [Host Mode] [Stop Meeting] ----
    int two_btn_total = BTN_W_HALF * 2 + BTN_GAP;
    int two_btn_x = (SCREEN_W - two_btn_total) / 2;

    s_btn_host = make_button(screen, "Host Mode",
                             two_btn_x, BTN_Y, BTN_W_HALF, BTN_H, btn_host_cb);

    s_btn_stop = make_button(screen, "Stop Meeting",
                             two_btn_x + BTN_W_HALF + BTN_GAP, BTN_Y,
                             BTN_W_HALF, BTN_H, btn_stop_cb);

    // ---- HOST: [Interrupt] (top) [Exit Host Mode] (bottom) ---- stacked vertically for round screen
    s_btn_interrupt = make_button(screen, "Interrupt",
                                   (SCREEN_W - BTN_W_WIDE) / 2, BTN_Y,
                                   BTN_W_WIDE, BTN_H, btn_interrupt_cb);

    s_btn_exit = make_button(screen, "Exit Host Mode",
                             (SCREEN_W - BTN_W_WIDE) / 2, BTN_Y + BTN_H + BTN_GAP,
                             BTN_W_WIDE, BTN_H, btn_exit_cb);

    // ---- WiFi settings: AP list ----
    s_wifi_ap_list = lv_list_create(screen);
    lv_obj_set_size(s_wifi_ap_list, 280, 200);
    lv_obj_align(s_wifi_ap_list, LV_ALIGN_TOP_MID, 0, 65);
    lv_obj_add_flag(s_wifi_ap_list, LV_OBJ_FLAG_HIDDEN);

    // ---- WiFi settings: selected SSID label ----
    s_wifi_ssid_label = lv_label_create(screen);
    lv_obj_set_style_text_font(s_wifi_ssid_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_wifi_ssid_label, lv_color_make(0xCC, 0xCC, 0xFF), 0);
    lv_label_set_text(s_wifi_ssid_label, "");
    lv_obj_set_width(s_wifi_ssid_label, 280);
    lv_obj_align(s_wifi_ssid_label, LV_ALIGN_TOP_MID, 0, 65);
    lv_obj_add_flag(s_wifi_ssid_label, LV_OBJ_FLAG_HIDDEN);

    // ---- WiFi settings: password textarea ----
    s_wifi_ta_pass = lv_textarea_create(screen);
    lv_obj_set_size(s_wifi_ta_pass, 280, 36);
    lv_obj_align(s_wifi_ta_pass, LV_ALIGN_TOP_MID, 0, 90);
    lv_textarea_set_placeholder_text(s_wifi_ta_pass, "Password");
    lv_textarea_set_max_length(s_wifi_ta_pass, 63);
    lv_textarea_set_one_line(s_wifi_ta_pass, true);
    lv_textarea_set_password_mode(s_wifi_ta_pass, true);
    lv_obj_add_flag(s_wifi_ta_pass, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_wifi_ta_pass, wifi_ta_focus_cb, LV_EVENT_FOCUSED, NULL);

    // ---- WiFi settings: Show/Hide password toggle ----
    s_wifi_btn_show = make_button(screen, "Show",
                                   170, 90, 70, 36, wifi_show_pass_cb);
    lv_obj_add_flag(s_wifi_btn_show, LV_OBJ_FLAG_HIDDEN);

    // ---- WiFi settings: status ----
    s_wifi_status = lv_label_create(screen);
    lv_obj_set_style_text_font(s_wifi_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_wifi_status, lv_color_make(0x60, 0xE0, 0x80), 0);
    lv_label_set_text(s_wifi_status, "");
    lv_obj_set_width(s_wifi_status, 280);
    lv_obj_align(s_wifi_status, LV_ALIGN_TOP_MID, 0, 135);
    lv_obj_add_flag(s_wifi_status, LV_OBJ_FLAG_HIDDEN);

    // ---- WiFi settings: keyboard ----
    s_wifi_kb = lv_keyboard_create(screen);
    lv_obj_set_size(s_wifi_kb, 280, 120);
    lv_obj_align(s_wifi_kb, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_keyboard_set_textarea(s_wifi_kb, s_wifi_ta_pass);
    lv_obj_add_flag(s_wifi_kb, LV_OBJ_FLAG_HIDDEN);

    // ---- WiFi settings: Save / Back (AFTER keyboard for higher z-order) ----
    s_wifi_btn_save = make_button(screen, "Save",
                                   40, 185, 120, 42, wifi_save_cb_internal);
    lv_obj_add_flag(s_wifi_btn_save, LV_OBJ_FLAG_HIDDEN);

    s_wifi_btn_back = make_button(screen, "Back",
                                   200, 185, 120, 42, wifi_back_cb_internal);
    lv_obj_add_flag(s_wifi_btn_back, LV_OBJ_FLAG_HIDDEN);

    // ---- Mic blink timer ----
    s_mic_timer = xTimerCreate("mic_blink", pdMS_TO_TICKS(500), pdTRUE, NULL, mic_timer_cb);
    if (s_mic_timer) xTimerStart(s_mic_timer, 0);

    // ---- Action text auto-clear timer ----
    const esp_timer_create_args_t action_clear_args = {
        .callback = action_clear_timer_cb,
        .name = "action_clear",
    };
    esp_timer_create(&action_clear_args, &s_action_clear_timer);

    ui_meeting_set_state(UI_STATE_IDLE);
}

// ---------------------------------------------------------------------------
// apply_state
// ---------------------------------------------------------------------------
static void hide_all_wifi(void)
{
    if (s_wifi_ap_list)     lv_obj_add_flag(s_wifi_ap_list, LV_OBJ_FLAG_HIDDEN);
    if (s_wifi_ssid_label)  lv_obj_add_flag(s_wifi_ssid_label, LV_OBJ_FLAG_HIDDEN);
    if (s_wifi_ta_pass)     lv_obj_add_flag(s_wifi_ta_pass, LV_OBJ_FLAG_HIDDEN);
    if (s_wifi_btn_show)    lv_obj_add_flag(s_wifi_btn_show, LV_OBJ_FLAG_HIDDEN);
    if (s_wifi_kb)          lv_obj_add_flag(s_wifi_kb, LV_OBJ_FLAG_HIDDEN);
    if (s_wifi_btn_save)    lv_obj_add_flag(s_wifi_btn_save, LV_OBJ_FLAG_HIDDEN);
    if (s_wifi_btn_back)    lv_obj_add_flag(s_wifi_btn_back, LV_OBJ_FLAG_HIDDEN);
    if (s_wifi_status)     lv_obj_add_flag(s_wifi_status, LV_OBJ_FLAG_HIDDEN);
}

static void apply_state(ui_state_t state)
{
    lv_obj_add_flag(s_btn_start,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_host,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_stop,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_exit,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_interrupt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_wifi,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_action_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_mic_dot,   LV_OBJ_FLAG_HIDDEN);
    hide_all_wifi();

    switch (state) {
    case UI_STATE_IDLE:
        lv_label_set_text(s_title_label, "Meeting Assistant");
        lv_obj_clear_flag(s_btn_start, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_btn_wifi,  LV_OBJ_FLAG_HIDDEN);
        if (s_action_clear_timer) esp_timer_stop(s_action_clear_timer);
        break;

    case UI_STATE_MEETING:
        lv_label_set_text(s_title_label, "Meeting in Progress");
        lv_obj_clear_flag(s_btn_host,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_btn_stop,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_mic_dot, LV_OBJ_FLAG_HIDDEN);
        break;

    case UI_STATE_HOST:
        lv_label_set_text(s_title_label, "Host Mode");
        lv_obj_clear_flag(s_btn_interrupt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_btn_exit,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
        break;

    case UI_STATE_WIFI_SETTINGS:
        lv_label_set_text(s_title_label, "WiFi Settings");
        // Show scan list, trigger scan
        lv_obj_clear_flag(s_wifi_ap_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_wifi_btn_back, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_wifi_status, "Scanning...");
        lv_obj_clear_flag(s_wifi_status, LV_OBJ_FLAG_HIDDEN);
        s_selected_ssid[0] = '\0';
        wifi_do_scan();
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
// ui_meeting_set_status / set_action_text
// ---------------------------------------------------------------------------
void ui_meeting_set_status(const char *text)
{
    if (!text) return;
    if (bsp_display_lock(100)) {
        lv_label_set_text(s_status_label, text);
        bsp_display_unlock();
    }
}

void ui_meeting_set_action_text(const char *text)
{
    if (!text) return;
    if (s_current_state != UI_STATE_HOST) return;
    s_action_generation++;
    if (bsp_display_lock(100)) {
        lv_label_set_text(s_action_label, text);
        lv_obj_clear_flag(s_action_label, LV_OBJ_FLAG_HIDDEN);
        bsp_display_unlock();
    }
    if (s_action_clear_timer) {
        esp_timer_stop(s_action_clear_timer);
        esp_timer_start_once(s_action_clear_timer, 5000000);
    }
}

// ---------------------------------------------------------------------------
// Button callbacks
// ---------------------------------------------------------------------------
static void btn_start_cb(lv_event_t *e)
{
    apply_state(UI_STATE_MEETING);
    lv_label_set_text(s_status_label, "Connecting...");
    lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    ws_session_start_meeting();
}

static void btn_host_cb(lv_event_t *e)
{
    apply_state(UI_STATE_HOST);
    lv_label_set_text(s_status_label, "Connecting...");
    lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    ws_session_enter_host();
}

static void btn_stop_cb(lv_event_t *e)
{
    apply_state(UI_STATE_IDLE);
    ws_session_stop_meeting();
}

static void btn_exit_cb(lv_event_t *e)
{
    apply_state(UI_STATE_MEETING);
    lv_label_set_text(s_status_label, "Reconnecting...");
    lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    ws_session_exit_host();
}

static void btn_interrupt_cb(lv_event_t *e)
{
    ws_session_interrupt();
}

static void btn_wifi_cb(lv_event_t *e)
{
    apply_state(UI_STATE_WIFI_SETTINGS);
}

static void wifi_ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (s_wifi_kb) lv_keyboard_set_textarea(s_wifi_kb, ta);
}

static void wifi_show_pass_cb(lv_event_t *e)
{
    if (!s_wifi_ta_pass) return;
    bool pwd_mode = lv_textarea_get_password_mode(s_wifi_ta_pass);
    lv_textarea_set_password_mode(s_wifi_ta_pass, !pwd_mode);
    lv_label_set_text(lv_obj_get_child(s_wifi_btn_show, 0),
                      pwd_mode ? "Hide" : "Show");
}

static void wifi_save_cb_internal(lv_event_t *e)
{
    // Close keyboard first to prevent input conflicts
    if (s_wifi_kb) lv_keyboard_set_textarea(s_wifi_kb, NULL);

    ESP_LOGI(TAG, "Save button clicked");
    char ssid[33] = {0};
    char pass[65] = {0};
    strlcpy(ssid, s_selected_ssid, sizeof(ssid));
    strlcpy(pass, lv_textarea_get_text(s_wifi_ta_pass), sizeof(pass));

    if (strlen(ssid) == 0) {
        ESP_LOGW(TAG, "Save: no SSID selected");
        lv_label_set_text(s_wifi_status, "No SSID selected");
        return;
    }

    ESP_LOGI(TAG, "saving WiFi: SSID=\"%s\" pass_len=%d", ssid, (int)strlen(pass));
    esp_err_t err = wifi_save_credentials(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
        lv_label_set_text(s_wifi_status, "Save failed");
        return;
    }

    // Verify by reading back
    char check_ssid[33] = {0};
    char check_pass[65] = {0};
    if (wifi_load_credentials(check_ssid, sizeof(check_ssid),
                              check_pass, sizeof(check_pass)) == ESP_OK) {
        ESP_LOGI(TAG, "NVS read-back OK: SSID=\"%s\"", check_ssid);
    } else {
        ESP_LOGE(TAG, "NVS read-back FAILED — credentials not persisted!");
    }

    lv_label_set_text(s_wifi_status, "Saved! Reconnecting...");
    strlcpy(s_current_ssid, ssid, sizeof(s_current_ssid));

    if (s_wifi_save_cb) {
        ESP_LOGI(TAG, "calling wifi_save_cb → reconnect");
        s_wifi_save_cb(ssid, pass);
    } else {
        ESP_LOGE(TAG, "wifi_save_cb is NULL!");
    }
}

static void wifi_back_cb_internal(lv_event_t *e)
{
    // If in password entry (SSID selected), go back to scan list
    if (s_selected_ssid[0] != '\0') {
        s_selected_ssid[0] = '\0';
        lv_obj_add_flag(s_wifi_ssid_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wifi_ta_pass, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wifi_btn_show, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wifi_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wifi_btn_save, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_wifi_ap_list, LV_OBJ_FLAG_HIDDEN);
    } else {
        apply_state(UI_STATE_IDLE);
    }
}

void ui_meeting_set_wifi_result(const char *text)
{
    if (s_wifi_status && s_current_state == UI_STATE_WIFI_SETTINGS) {
        if (bsp_display_lock(100)) {
            lv_label_set_text(s_wifi_status, text);
            bsp_display_unlock();
        }
    }
}
