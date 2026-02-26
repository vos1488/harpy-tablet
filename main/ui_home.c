/*
 * Home Screen UI - Tablet Launcher
 * 
 * Layout:
 * - Top status bar: WiFi icon, BT icon, IP, time
 * - Main area: App grid (WiFi Settings, BT Scanner, Stream Viewer, Settings)
 * - Each "app" opens its own screen
 */

#include "ui_home.h"
#include "ui_keyboard.h"
#include "ui_settings.h"
#include "ui_sdcard.h"
#include "ui_rs485.h"
#include "ui_adc.h"
#include "ui_can.h"
#include "ui_network.h"
#include "ui_games.h"
#include "ui_carplay.h"
#include "wifi_manager.h"
#include "bt_manager.h"
#include "stream_viewer.h"
#include "harpy_config.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "ui_home";

/* ==================== Global UI elements ==================== */
static lv_obj_t *s_home_screen = NULL;
static lv_obj_t *s_status_bar = NULL;
static lv_obj_t *s_lbl_wifi_icon = NULL;
static lv_obj_t *s_lbl_bt_icon = NULL;
static lv_obj_t *s_lbl_ip = NULL;
static lv_obj_t *s_lbl_time = NULL;
static lv_obj_t *s_content_area = NULL;

/* Sub-screens */
static lv_obj_t *s_wifi_screen = NULL;
static lv_obj_t *s_bt_screen = NULL;
static lv_obj_t *s_stream_screen = NULL;

/* WiFi scan list */
static lv_obj_t *s_wifi_list = NULL;
static char s_selected_ssid[33] = {};

/* Thread-safe scan result storage */
static wifi_ap_info_t s_wifi_scan_results[20];
static uint16_t s_wifi_scan_count = 0;
static volatile bool s_wifi_scan_pending = false;

static bt_device_info_t s_bt_scan_results[30];
static uint16_t s_bt_scan_count = 0;
static volatile bool s_bt_scan_pending = false;

/* Stream UI */
static lv_obj_t *s_stream_img = NULL;
static lv_obj_t *s_stream_status_lbl = NULL;
static char s_stream_ip[64] = "192.168.1.100";
static char s_stream_port[8] = "8080";
static char s_stream_path[64] = "/";
static lv_timer_t *s_stream_refresh_timer = NULL;

/* ==================== Style Helpers ==================== */

static lv_style_t style_card;
static lv_style_t style_btn;
static lv_style_t style_btn_pressed;
static bool styles_inited = false;

static void init_styles(void)
{
    if (styles_inited) return;
    styles_inited = true;

    /* Card style */
    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, HARPY_COLOR_CARD);
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_radius(&style_card, 16);
    lv_style_set_border_width(&style_card, 0);
    lv_style_set_pad_all(&style_card, 16);
    lv_style_set_shadow_width(&style_card, 20);
    lv_style_set_shadow_opa(&style_card, LV_OPA_20);
    lv_style_set_shadow_color(&style_card, lv_color_black());

    /* Button style */
    lv_style_init(&style_btn);
    lv_style_set_bg_color(&style_btn, HARPY_COLOR_PRIMARY);
    lv_style_set_bg_opa(&style_btn, LV_OPA_COVER);
    lv_style_set_radius(&style_btn, 12);
    lv_style_set_text_color(&style_btn, lv_color_white());
    lv_style_set_pad_hor(&style_btn, 20);
    lv_style_set_pad_ver(&style_btn, 12);

    lv_style_init(&style_btn_pressed);
    lv_style_set_bg_color(&style_btn_pressed, HARPY_COLOR_ACCENT);
}

/* ==================== Back Button Helper ==================== */

static void back_btn_cb(lv_event_t *e)
{
    lv_obj_t *screen = lv_event_get_user_data(e);
    if (screen) {
        lv_obj_del(screen);
    }
    /* Restore home */
    lv_scr_load(s_home_screen);
}

static lv_obj_t *create_sub_screen(const char *title, lv_obj_t **out_content)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, HARPY_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Top bar */
    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LCD_H_RES, 50);
    lv_obj_set_style_bg_color(top, HARPY_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);

    /* Back button */
    lv_obj_t *btn_back = lv_btn_create(top);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_bg_color(btn_back, HARPY_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(btn_back, 8, 0);
    lv_obj_add_event_cb(btn_back, back_btn_cb, LV_EVENT_CLICKED, scr);

    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(lbl_back, lv_color_white(), 0);
    lv_obj_center(lbl_back);

    /* Title */
    lv_obj_t *lbl_title = lv_label_create(top);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_title, HARPY_COLOR_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, 0);

    /* Content area */
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, LCD_H_RES, LCD_V_RES - 50);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(content, 10, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (out_content) *out_content = content;
    return scr;
}

/* ==================== WiFi Settings Screen ==================== */

/* Forward declarations */
static void wifi_ap_connect_cb(lv_event_t *e);
static void wifi_password_enter_cb(const char *text);

static void wifi_scan_result_cb(wifi_ap_info_t *ap_list, uint16_t count)
{
    /* Store results — will be processed by LVGL timer on the LVGL task */
    if (count > 20) count = 20;
    memcpy(s_wifi_scan_results, ap_list, count * sizeof(wifi_ap_info_t));
    s_wifi_scan_count = count;
    s_wifi_scan_pending = true;
}

/* Called from LVGL context to safely update WiFi list */
static void wifi_scan_update_ui(void)
{
    if (!s_wifi_list || !s_wifi_scan_pending) return;
    s_wifi_scan_pending = false;

    uint16_t count = s_wifi_scan_count;
    wifi_ap_info_t *ap_list = s_wifi_scan_results;

    lv_obj_clean(s_wifi_list);

    for (int i = 0; i < count; i++) {
        /* Create row for each AP */
        lv_obj_t *row = lv_obj_create(s_wifi_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 50);
        lv_obj_add_style(row, &style_card, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        /* Signal icon */
        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(icon, 
            ap_list[i].rssi > -50 ? HARPY_COLOR_SUCCESS : 
            (ap_list[i].rssi > -70 ? HARPY_COLOR_WARN : HARPY_COLOR_ERROR), 0);

        /* SSID + RSSI */
        lv_obj_t *info = lv_obj_create(row);
        lv_obj_remove_style_all(info);
        lv_obj_set_flex_grow(info, 1);
        lv_obj_set_height(info, 40);

        lv_obj_t *ssid_lbl = lv_label_create(info);
        char txt[64];
        snprintf(txt, sizeof(txt), "%s  (%ddBm)", ap_list[i].ssid, ap_list[i].rssi);
        lv_label_set_text(ssid_lbl, txt);
        lv_obj_set_style_text_color(ssid_lbl, HARPY_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_16, 0);

        /* Lock icon if encrypted */
        if (ap_list[i].auth != WIFI_AUTH_OPEN) {
            lv_obj_t *lock = lv_label_create(row);
            lv_label_set_text(lock, LV_SYMBOL_CLOSE);
            lv_obj_set_style_text_color(lock, HARPY_COLOR_MUTED, 0);
        }

        /* Connect button */
        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_set_size(btn, 90, 36);
        lv_obj_add_style(btn, &style_btn, 0);
        lv_obj_set_style_radius(btn, 8, 0);

        lv_obj_t *btn_lbl = lv_label_create(btn);
        lv_label_set_text(btn_lbl, "Connect");
        lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(btn_lbl);

        /* Store SSID for the callback */
        char *ssid_copy = lv_mem_alloc(33);
        strncpy(ssid_copy, ap_list[i].ssid, 32);
        ssid_copy[32] = '\0';
        lv_obj_set_user_data(btn, ssid_copy);
        lv_obj_add_event_cb(btn, wifi_ap_connect_cb, LV_EVENT_CLICKED, NULL);
    }
}

static void wifi_ap_connect_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    char *ssid = lv_obj_get_user_data(btn);
    if (ssid) {
        strncpy(s_selected_ssid, ssid, sizeof(s_selected_ssid) - 1);
        ui_keyboard_show(lv_scr_act(), "WiFi Password", NULL, wifi_password_enter_cb);
    }
}

static void wifi_password_enter_cb(const char *text)
{
    wifi_manager_connect(s_selected_ssid, text);
    wifi_manager_save_credentials(s_selected_ssid, text);
}

static void wifi_scan_btn_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Scanning WiFi...");
    wifi_manager_scan(wifi_scan_result_cb);
}

static void wifi_disconnect_btn_cb(lv_event_t *e)
{
    wifi_manager_disconnect();
}

static void open_wifi_screen(lv_event_t *e)
{
    lv_obj_t *content = NULL;
    s_wifi_screen = create_sub_screen(LV_SYMBOL_WIFI "  WiFi Settings", &content);

    /* Status info */
    lv_obj_t *status_panel = lv_obj_create(content);
    lv_obj_remove_style_all(status_panel);
    lv_obj_set_size(status_panel, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_add_style(status_panel, &style_card, 0);
    lv_obj_set_flex_flow(status_panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_panel, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* WiFi state label */
    lv_obj_t *state_lbl = lv_label_create(status_panel);
    wifi_state_t st = wifi_manager_get_state();
    const char *state_txt = st == WIFI_STATE_CONNECTED ? "Connected" :
                            st == WIFI_STATE_CONNECTING ? "Connecting..." :
                            st == WIFI_STATE_FAILED ? "Failed" : "Disconnected";
    char state_str[80];
    snprintf(state_str, sizeof(state_str), "Status: %s   IP: %s", state_txt, wifi_manager_get_ip());
    lv_label_set_text(state_lbl, state_str);
    lv_obj_set_style_text_color(state_lbl, HARPY_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(state_lbl, &lv_font_montserrat_16, 0);

    /* Buttons row */
    lv_obj_t *btn_row = lv_obj_create(content);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, lv_pct(100), 50);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 10, 0);

    lv_obj_t *scan_btn = lv_btn_create(btn_row);
    lv_obj_add_style(scan_btn, &style_btn, 0);
    lv_obj_add_event_cb(scan_btn, wifi_scan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_lbl = lv_label_create(scan_btn);
    lv_label_set_text(scan_lbl, LV_SYMBOL_REFRESH "  Scan");
    lv_obj_center(scan_lbl);

    lv_obj_t *dc_btn = lv_btn_create(btn_row);
    lv_obj_set_style_bg_color(dc_btn, HARPY_COLOR_ERROR, 0);
    lv_obj_set_style_radius(dc_btn, 12, 0);
    lv_obj_set_style_pad_hor(dc_btn, 20, 0);
    lv_obj_set_style_pad_ver(dc_btn, 12, 0);
    lv_obj_add_event_cb(dc_btn, wifi_disconnect_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dc_lbl = lv_label_create(dc_btn);
    lv_label_set_text(dc_lbl, "Disconnect");
    lv_obj_set_style_text_color(dc_lbl, lv_color_white(), 0);
    lv_obj_center(dc_lbl);

    /* Scan results list */
    s_wifi_list = lv_obj_create(content);
    lv_obj_remove_style_all(s_wifi_list);
    lv_obj_set_size(s_wifi_list, lv_pct(100), LCD_V_RES - 200);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_wifi_list, 6, 0);
    lv_obj_add_flag(s_wifi_list, LV_OBJ_FLAG_SCROLLABLE);

    /* Load and auto-scan */
    lv_scr_load(s_wifi_screen);
    wifi_manager_scan(wifi_scan_result_cb);
}

/* ==================== Bluetooth Screen ==================== */

static lv_obj_t *s_bt_list = NULL;
static lv_obj_t *s_bt_conn_status = NULL;

static void bt_scan_result_cb(bt_device_info_t *devices, uint16_t count)
{
    /* Store results — will be processed by LVGL timer on the LVGL task */
    if (count > 30) count = 30;
    memcpy(s_bt_scan_results, devices, count * sizeof(bt_device_info_t));
    s_bt_scan_count = count;
    s_bt_scan_pending = true;
}

/* Callback for connect button on each device row */
static void bt_row_connect_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_bt_scan_count) return;

    bt_device_info_t *dev = &s_bt_scan_results[idx];
    bt_manager_connect_device(dev->addr, dev->addr_type);

    if (s_bt_conn_status) {
        char buf[96];
        snprintf(buf, sizeof(buf), LV_SYMBOL_REFRESH "  Connecting to %s...", dev->name);
        lv_label_set_text(s_bt_conn_status, buf);
        lv_obj_set_style_text_color(s_bt_conn_status, HARPY_COLOR_WARN, 0);
    }
}

/* Callback for disconnect button */
static void bt_disconnect_cb(lv_event_t *e)
{
    bt_manager_disconnect_device();
    if (s_bt_conn_status) {
        lv_label_set_text(s_bt_conn_status, LV_SYMBOL_CLOSE "  Disconnected");
        lv_obj_set_style_text_color(s_bt_conn_status, HARPY_COLOR_MUTED, 0);
    }
}

/* Connection state callback (called from BLE context) */
static volatile bool s_bt_conn_changed = false;
static volatile bool s_bt_conn_result = false;
static char s_bt_conn_peer[BT_DEVICE_NAME_MAX] = "";

static void bt_conn_state_cb(bool connected, const char *name)
{
    if (name && name[0]) {
        strncpy(s_bt_conn_peer, name, BT_DEVICE_NAME_MAX - 1);
    }
    s_bt_conn_result = connected;
    s_bt_conn_changed = true;
}

/* Poll connection state in LVGL context */
static void bt_conn_poll_ui(void)
{
    if (!s_bt_conn_changed || !s_bt_conn_status) return;
    s_bt_conn_changed = false;

    if (s_bt_conn_result) {
        char buf[96];
        snprintf(buf, sizeof(buf), LV_SYMBOL_OK "  Connected: %s", s_bt_conn_peer);
        lv_label_set_text(s_bt_conn_status, buf);
        lv_obj_set_style_text_color(s_bt_conn_status, HARPY_COLOR_SUCCESS, 0);
    } else {
        lv_label_set_text(s_bt_conn_status, LV_SYMBOL_CLOSE "  Disconnected");
        lv_obj_set_style_text_color(s_bt_conn_status, HARPY_COLOR_MUTED, 0);
    }
}

/* Called from LVGL context to safely update BT list */
static void bt_scan_update_ui(void)
{
    bt_conn_poll_ui();

    if (!s_bt_list || !s_bt_scan_pending) return;
    s_bt_scan_pending = false;

    uint16_t count = s_bt_scan_count;
    bt_device_info_t *devices = s_bt_scan_results;

    lv_obj_clean(s_bt_list);

    for (int i = 0; i < count; i++) {
        lv_obj_t *row = lv_obj_create(s_bt_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 45);
        lv_obj_add_style(row, &style_card, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_color(icon, HARPY_COLOR_PRIMARY, 0);

        lv_obj_t *name_lbl = lv_label_create(row);
        char txt[96];
        snprintf(txt, sizeof(txt), "%s  (%d dBm)", devices[i].name, devices[i].rssi);
        lv_label_set_text(name_lbl, txt);
        lv_obj_set_style_text_color(name_lbl, HARPY_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_flex_grow(name_lbl, 1);

        /* RSSI bar */
        lv_obj_t *bar = lv_bar_create(row);
        lv_obj_set_size(bar, 50, 10);
        lv_bar_set_range(bar, -100, 0);
        lv_bar_set_value(bar, devices[i].rssi, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, HARPY_COLOR_CARD, 0);
        lv_obj_set_style_bg_color(bar, HARPY_COLOR_SUCCESS, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 5, 0);
        lv_obj_set_style_radius(bar, 5, LV_PART_INDICATOR);

        /* Connect button */
        lv_obj_t *conn_btn = lv_btn_create(row);
        lv_obj_set_size(conn_btn, 80, 30);
        lv_obj_set_style_bg_color(conn_btn, HARPY_COLOR_ACCENT, 0);
        lv_obj_set_style_radius(conn_btn, 8, 0);
        lv_obj_set_style_pad_all(conn_btn, 0, 0);
        lv_obj_add_event_cb(conn_btn, bt_row_connect_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *cb_lbl = lv_label_create(conn_btn);
        lv_label_set_text(cb_lbl, "Connect");
        lv_obj_set_style_text_color(cb_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(cb_lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(cb_lbl);
    }
}

static void bt_scan_btn_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Scanning BLE...");
    bt_manager_scan(bt_scan_result_cb);
}

static void open_bt_screen(lv_event_t *e)
{
    lv_obj_t *content = NULL;
    s_bt_screen = create_sub_screen(LV_SYMBOL_BLUETOOTH "  Bluetooth", &content);

    /* Top row: Scan + Disconnect buttons */
    lv_obj_t *btn_row = lv_obj_create(content);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 10, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *scan_btn = lv_btn_create(btn_row);
    lv_obj_add_style(scan_btn, &style_btn, 0);
    lv_obj_set_width(scan_btn, 180);
    lv_obj_add_event_cb(scan_btn, bt_scan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_lbl = lv_label_create(scan_btn);
    lv_label_set_text(scan_lbl, LV_SYMBOL_REFRESH "  Scan");
    lv_obj_center(scan_lbl);

    lv_obj_t *disc_btn = lv_btn_create(btn_row);
    lv_obj_set_size(disc_btn, 180, 40);
    lv_obj_set_style_bg_color(disc_btn, HARPY_COLOR_ERROR, 0);
    lv_obj_set_style_radius(disc_btn, 12, 0);
    lv_obj_add_event_cb(disc_btn, bt_disconnect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *disc_lbl = lv_label_create(disc_btn);
    lv_label_set_text(disc_lbl, LV_SYMBOL_CLOSE "  Disconnect");
    lv_obj_set_style_text_color(disc_lbl, lv_color_white(), 0);
    lv_obj_center(disc_lbl);

    /* Connection status */
    s_bt_conn_status = lv_label_create(btn_row);
    if (bt_manager_is_device_connected()) {
        char buf[96];
        snprintf(buf, sizeof(buf), LV_SYMBOL_OK "  Connected: %s",
                 bt_manager_connected_device_name());
        lv_label_set_text(s_bt_conn_status, buf);
        lv_obj_set_style_text_color(s_bt_conn_status, HARPY_COLOR_SUCCESS, 0);
    } else {
        lv_label_set_text(s_bt_conn_status, "No device connected");
        lv_obj_set_style_text_color(s_bt_conn_status, HARPY_COLOR_MUTED, 0);
    }
    lv_obj_set_style_text_font(s_bt_conn_status, &lv_font_montserrat_14, 0);

    /* Set connection callback */
    bt_manager_set_conn_cb(bt_conn_state_cb);

    s_bt_list = lv_obj_create(content);
    lv_obj_remove_style_all(s_bt_list);
    lv_obj_set_size(s_bt_list, lv_pct(100), LCD_V_RES - 180);
    lv_obj_set_flex_flow(s_bt_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_bt_list, 6, 0);
    lv_obj_add_flag(s_bt_list, LV_OBJ_FLAG_SCROLLABLE);

    lv_scr_load(s_bt_screen);
    bt_manager_scan(bt_scan_result_cb);
}

/* ==================== Stream Viewer Screen ==================== */

static void stream_refresh_timer_cb(lv_timer_t *timer)
{
    if (!s_stream_img) return;

    lv_img_dsc_t *frame = stream_viewer_get_frame();
    if (frame && frame->data) {
        lv_img_set_src(s_stream_img, frame);
    }

    /* Update status + FPS */
    if (s_stream_status_lbl) {
        stream_state_t st = stream_viewer_get_state();
        if (st <= STREAM_STATE_ERROR) {
            char status_txt[64];
            if (st == STREAM_STATE_STREAMING) {
                float fps = stream_viewer_get_fps();
                snprintf(status_txt, sizeof(status_txt), "Streaming  %.1f FPS", fps);
            } else {
                const char *names[] = {"Idle", "Connecting...", "Streaming", "Error"};
                snprintf(status_txt, sizeof(status_txt), "%s", names[st]);
            }
            lv_label_set_text(s_stream_status_lbl, status_txt);
            lv_color_t clr = st == STREAM_STATE_STREAMING ? HARPY_COLOR_SUCCESS :
                             st == STREAM_STATE_ERROR ? HARPY_COLOR_ERROR :
                             st == STREAM_STATE_CONNECTING ? HARPY_COLOR_WARN : HARPY_COLOR_MUTED;
            lv_obj_set_style_text_color(s_stream_status_lbl, clr, 0);
        }
    }
}

/* Callbacks for IP/port/path input */
static void stream_ip_entered(const char *text)
{
    strncpy(s_stream_ip, text, sizeof(s_stream_ip) - 1);
}

static void stream_port_entered(const char *text)
{
    strncpy(s_stream_port, text, sizeof(s_stream_port) - 1);
}

static void stream_path_entered(const char *text)
{
    strncpy(s_stream_path, text, sizeof(s_stream_path) - 1);
}

static void stream_ip_btn_cb(lv_event_t *e)
{
    ui_keyboard_show(lv_scr_act(), "Stream IP Address", s_stream_ip, stream_ip_entered);
}

static void stream_port_btn_cb(lv_event_t *e)
{
    ui_keyboard_show(lv_scr_act(), "Stream Port", s_stream_port, stream_port_entered);
}

static void stream_path_btn_cb(lv_event_t *e)
{
    ui_keyboard_show(lv_scr_act(), "Stream Path (e.g. /mjpeg)", s_stream_path, stream_path_entered);
}

static void stream_start_btn_cb(lv_event_t *e)
{
    char url[256];
    snprintf(url, sizeof(url), "http://%s:%s%s", s_stream_ip, s_stream_port, s_stream_path);
    ESP_LOGI(TAG, "Starting stream: %s", url);
    stream_viewer_start(url);
}

static void stream_stop_btn_cb(lv_event_t *e)
{
    stream_viewer_stop();
}

static void open_stream_screen(lv_event_t *e)
{
    lv_obj_t *content = NULL;
    s_stream_screen = create_sub_screen(LV_SYMBOL_VIDEO "  Stream Viewer", &content);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 6, 0);

    /* Connection settings panel */
    lv_obj_t *settings_panel = lv_obj_create(content);
    lv_obj_remove_style_all(settings_panel);
    lv_obj_set_size(settings_panel, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_add_style(settings_panel, &style_card, 0);
    lv_obj_set_flex_flow(settings_panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(settings_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(settings_panel, 8, 0);
    lv_obj_clear_flag(settings_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* IP button */
    lv_obj_t *ip_btn = lv_btn_create(settings_panel);
    lv_obj_set_style_bg_color(ip_btn, HARPY_COLOR_CARD, 0);
    lv_obj_set_style_border_color(ip_btn, HARPY_COLOR_PRIMARY, 0);
    lv_obj_set_style_border_width(ip_btn, 1, 0);
    lv_obj_set_style_radius(ip_btn, 8, 0);
    lv_obj_set_style_pad_all(ip_btn, 8, 0);
    lv_obj_add_event_cb(ip_btn, stream_ip_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ip_lbl = lv_label_create(ip_btn);
    char ip_txt[80];
    snprintf(ip_txt, sizeof(ip_txt), "IP: %s", s_stream_ip);
    lv_label_set_text(ip_lbl, ip_txt);
    lv_obj_set_style_text_color(ip_lbl, HARPY_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(ip_lbl, &lv_font_montserrat_14, 0);

    /* Port button */
    lv_obj_t *port_btn = lv_btn_create(settings_panel);
    lv_obj_set_style_bg_color(port_btn, HARPY_COLOR_CARD, 0);
    lv_obj_set_style_border_color(port_btn, HARPY_COLOR_PRIMARY, 0);
    lv_obj_set_style_border_width(port_btn, 1, 0);
    lv_obj_set_style_radius(port_btn, 8, 0);
    lv_obj_set_style_pad_all(port_btn, 8, 0);
    lv_obj_add_event_cb(port_btn, stream_port_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *port_lbl = lv_label_create(port_btn);
    char port_txt[32];
    snprintf(port_txt, sizeof(port_txt), "Port: %s", s_stream_port);
    lv_label_set_text(port_lbl, port_txt);
    lv_obj_set_style_text_color(port_lbl, HARPY_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(port_lbl, &lv_font_montserrat_14, 0);

    /* Path button */
    lv_obj_t *path_btn = lv_btn_create(settings_panel);
    lv_obj_set_style_bg_color(path_btn, HARPY_COLOR_CARD, 0);
    lv_obj_set_style_border_color(path_btn, HARPY_COLOR_PRIMARY, 0);
    lv_obj_set_style_border_width(path_btn, 1, 0);
    lv_obj_set_style_radius(path_btn, 8, 0);
    lv_obj_set_style_pad_all(path_btn, 8, 0);
    lv_obj_add_event_cb(path_btn, stream_path_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *path_lbl = lv_label_create(path_btn);
    char path_txt[80];
    snprintf(path_txt, sizeof(path_txt), "Path: %s", s_stream_path);
    lv_label_set_text(path_lbl, path_txt);
    lv_obj_set_style_text_color(path_lbl, HARPY_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(path_lbl, &lv_font_montserrat_14, 0);

    /* Start/Stop buttons */
    lv_obj_t *start_btn = lv_btn_create(settings_panel);
    lv_obj_add_style(start_btn, &style_btn, 0);
    lv_obj_set_style_bg_color(start_btn, HARPY_COLOR_SUCCESS, 0);
    lv_obj_add_event_cb(start_btn, stream_start_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *start_lbl = lv_label_create(start_btn);
    lv_label_set_text(start_lbl, LV_SYMBOL_PLAY " Start");
    lv_obj_center(start_lbl);

    lv_obj_t *stop_btn = lv_btn_create(settings_panel);
    lv_obj_set_style_bg_color(stop_btn, HARPY_COLOR_ERROR, 0);
    lv_obj_set_style_radius(stop_btn, 12, 0);
    lv_obj_set_style_pad_hor(stop_btn, 20, 0);
    lv_obj_set_style_pad_ver(stop_btn, 12, 0);
    lv_obj_add_event_cb(stop_btn, stream_stop_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *stop_lbl = lv_label_create(stop_btn);
    lv_label_set_text(stop_lbl, LV_SYMBOL_STOP " Stop");
    lv_obj_set_style_text_color(stop_lbl, lv_color_white(), 0);
    lv_obj_center(stop_lbl);

    /* Status label */
    s_stream_status_lbl = lv_label_create(content);
    lv_label_set_text(s_stream_status_lbl, "Idle");
    lv_obj_set_style_text_color(s_stream_status_lbl, HARPY_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_stream_status_lbl, &lv_font_montserrat_14, 0);

    /* Stream image display area */
    lv_obj_t *img_container = lv_obj_create(content);
    lv_obj_remove_style_all(img_container);
    lv_obj_set_size(img_container, lv_pct(100), LCD_V_RES - 190);
    lv_obj_set_style_bg_color(img_container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(img_container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(img_container, 12, 0);
    lv_obj_set_style_clip_corner(img_container, true, 0);

    s_stream_img = lv_img_create(img_container);
    lv_obj_center(s_stream_img);

    /* No stream placeholder */
    lv_obj_t *placeholder = lv_label_create(img_container);
    lv_label_set_text(placeholder, "No stream\nEnter IP:Port and press Start");
    lv_obj_set_style_text_color(placeholder, HARPY_COLOR_MUTED, 0);
    lv_obj_set_style_text_align(placeholder, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(placeholder, &lv_font_montserrat_16, 0);
    lv_obj_center(placeholder);

    /* Frame refresh timer (30 Hz) */
    s_stream_refresh_timer = lv_timer_create(stream_refresh_timer_cb, 33, NULL);

    lv_scr_load(s_stream_screen);
}

/* ==================== Settings Screen ==================== */

static void open_settings_screen(lv_event_t *e)
{
    ui_settings_open();
}

static void open_sdcard_screen(lv_event_t *e)
{
    ui_sdcard_open();
}

static void open_rs485_screen(lv_event_t *e)
{
    ui_rs485_open();
}

static void open_adc_screen(lv_event_t *e)
{
    ui_adc_open();
}

static void open_can_screen(lv_event_t *e)
{
    ui_can_open();
}

static void open_network_screen(lv_event_t *e)
{
    ui_network_open();
}

static void open_games_screen(lv_event_t *e)
{
    ui_games_open();
}

static void open_carplay_screen(lv_event_t *e)
{
    ui_carplay_open();
}

/* ==================== Scan UI Update Timer ==================== */
/* Checks for pending scan results and updates LVGL safely from the LVGL task */

static void scan_poll_timer_cb(lv_timer_t *timer)
{
    wifi_scan_update_ui();
    bt_scan_update_ui();
}

/* ==================== Home Screen ==================== */

static lv_obj_t *create_app_icon(lv_obj_t *parent, const char *icon,
                                  const char *name, lv_color_t color,
                                  lv_event_cb_t click_cb, bool dock_item)
{
    int icon_sz = dock_item ? 48 : 56;
    int icon_rad = dock_item ? 12 : 14;

    /* Outer container — transparent, just for layout */
    lv_obj_t *ctr = lv_obj_create(parent);
    lv_obj_remove_style_all(ctr);
    lv_obj_set_size(ctr, dock_item ? 70 : 85, dock_item ? 66 : 88);
    lv_obj_set_flex_flow(ctr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ctr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ctr, 6, 0);
    lv_obj_clear_flag(ctr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ctr, LV_OBJ_FLAG_CLICKABLE);

    /* Pressed effect — scale-down illusion */
    lv_obj_set_style_opa(ctr, LV_OPA_60, LV_STATE_PRESSED);

    /* Rounded-square icon with solid color (iOS-style) */
    lv_obj_t *icon_bg = lv_obj_create(ctr);
    lv_obj_remove_style_all(icon_bg);
    lv_obj_set_size(icon_bg, icon_sz, icon_sz);
    lv_obj_set_style_bg_color(icon_bg, color, 0);
    lv_obj_set_style_bg_opa(icon_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon_bg, icon_rad, 0);
    lv_obj_set_style_shadow_width(icon_bg, 15, 0);
    lv_obj_set_style_shadow_opa(icon_bg, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(icon_bg, color, 0);
    lv_obj_set_style_shadow_ofs_y(icon_bg, 4, 0);
    lv_obj_clear_flag(icon_bg, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *icon_lbl = lv_label_create(icon_bg);
    lv_label_set_text(icon_lbl, icon);
    lv_obj_set_style_text_color(icon_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(icon_lbl, dock_item ? &lv_font_montserrat_24 : &lv_font_montserrat_28, 0);
    lv_obj_center(icon_lbl);

    /* Name label */
    lv_obj_t *name_lbl = lv_label_create(ctr);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_style_text_color(name_lbl, HARPY_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);

    if (click_cb) {
        lv_obj_add_event_cb(ctr, click_cb, LV_EVENT_CLICKED, NULL);
    }

    return ctr;
}

void ui_home_create(lv_obj_t *parent)
{
    init_styles();

    s_home_screen = parent;
    /* ==================== Wallpaper Background ==================== */
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x0a0e17), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_color(parent, lv_color_hex(0x1a1040), 0);
    lv_obj_set_style_bg_grad_dir(parent, LV_GRAD_DIR_VER, 0);

    /* ==================== Status Bar (28px, iOS-style) ==================== */
    s_status_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(s_status_bar);
    lv_obj_set_size(s_status_bar, LCD_H_RES, 28);
    lv_obj_set_style_bg_color(s_status_bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_40, 0);
    lv_obj_align(s_status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(s_status_bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Left: WiFi + BT indicators */
    lv_obj_t *left_grp = lv_obj_create(s_status_bar);
    lv_obj_remove_style_all(left_grp);
    lv_obj_set_size(left_grp, LV_SIZE_CONTENT, 28);
    lv_obj_set_flex_flow(left_grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_grp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left_grp, 8, 0);
    lv_obj_align(left_grp, LV_ALIGN_LEFT_MID, 14, 0);

    lv_obj_t *brand = lv_label_create(left_grp);
    lv_label_set_text(brand, "HARPY");
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(brand, HARPY_COLOR_PRIMARY, 0);
    lv_obj_set_style_text_letter_space(brand, 2, 0);

    s_lbl_wifi_icon = lv_label_create(left_grp);
    lv_label_set_text(s_lbl_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_lbl_wifi_icon, HARPY_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_lbl_wifi_icon, &lv_font_montserrat_14, 0);

    s_lbl_bt_icon = lv_label_create(left_grp);
    lv_label_set_text(s_lbl_bt_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_color(s_lbl_bt_icon, HARPY_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_lbl_bt_icon, &lv_font_montserrat_14, 0);

    /* Center: Time (iOS-style) */
    s_lbl_time = lv_label_create(s_status_bar);
    lv_label_set_text(s_lbl_time, "--:--");
    lv_obj_set_style_text_color(s_lbl_time, HARPY_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_lbl_time, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_time, LV_ALIGN_CENTER, 0, 0);

    /* Right: IP address */
    s_lbl_ip = lv_label_create(s_status_bar);
    lv_label_set_text(s_lbl_ip, "Not connected");
    lv_obj_set_style_text_color(s_lbl_ip, HARPY_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_lbl_ip, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_ip, LV_ALIGN_RIGHT_MID, -14, 0);

    /* ==================== App Grid Area ==================== */
    /* Main grid with 5 apps, centered in available space above dock */
    s_content_area = lv_obj_create(parent);
    lv_obj_remove_style_all(s_content_area);
    lv_obj_set_size(s_content_area, LCD_H_RES, LCD_V_RES - 28 - 80);
    lv_obj_align(s_content_area, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_flex_flow(s_content_area, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_content_area, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_content_area, 35, 0);
    lv_obj_set_style_pad_row(s_content_area, 20, 0);
    lv_obj_clear_flag(s_content_area, LV_OBJ_FLAG_SCROLLABLE);

    /* Grid apps (tools & peripherals) */
    create_app_icon(s_content_area, LV_SYMBOL_BLUETOOTH, "Bluetooth",
                    HARPY_COLOR_ACCENT, open_bt_screen, false);

    create_app_icon(s_content_area, LV_SYMBOL_VIDEO, "Stream",
                    HARPY_COLOR_SUCCESS, open_stream_screen, false);

    create_app_icon(s_content_area, LV_SYMBOL_CALL, "RS485",
                    lv_color_hex(0xF97316), open_rs485_screen, false);

    create_app_icon(s_content_area, LV_SYMBOL_CHARGE, "Sensor",
                    lv_color_hex(0x06B6D4), open_adc_screen, false);

    create_app_icon(s_content_area, LV_SYMBOL_LOOP, "CAN Bus",
                    lv_color_hex(0xA855F7), open_can_screen, false);

    create_app_icon(s_content_area, LV_SYMBOL_PLAY, "Games",
                    lv_color_hex(0xF97316), open_games_screen, false);

    create_app_icon(s_content_area, LV_SYMBOL_AUDIO, "CarPlay",
                    lv_color_hex(0x0A84FF), open_carplay_screen, false);

    /* ==================== Dock Bar (frosted glass style) ==================== */
    lv_obj_t *dock = lv_obj_create(parent);
    lv_obj_remove_style_all(dock);
    lv_obj_set_size(dock, 500, 72);
    lv_obj_set_style_bg_color(dock, lv_color_hex(0x1C2333), 0);
    lv_obj_set_style_bg_opa(dock, LV_OPA_60, 0);
    lv_obj_set_style_radius(dock, 24, 0);
    lv_obj_set_style_border_width(dock, 1, 0);
    lv_obj_set_style_border_color(dock, lv_color_hex(0x2D3748), 0);
    lv_obj_set_style_border_opa(dock, LV_OPA_40, 0);
    lv_obj_align(dock, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(dock, 20, 0);
    lv_obj_clear_flag(dock, LV_OBJ_FLAG_SCROLLABLE);

    /* Dock apps (essentials) */
    create_app_icon(dock, LV_SYMBOL_WIFI, "WiFi",
                    HARPY_COLOR_PRIMARY, open_wifi_screen, true);

    create_app_icon(dock, LV_SYMBOL_DRIVE, "SD Card",
                    lv_color_hex(0x3B82F6), open_sdcard_screen, true);

    create_app_icon(dock, LV_SYMBOL_SETTINGS, "Settings",
                    HARPY_COLOR_WARN, open_settings_screen, true);

    create_app_icon(dock, LV_SYMBOL_SHUFFLE, "Network",
                    lv_color_hex(0x10B981), open_network_screen, true);

    /* Start scan result poll timer (runs in LVGL task context) */
    lv_timer_create(scan_poll_timer_cb, 200, NULL);

    ESP_LOGI(TAG, "Home screen created (tablet OS)");
}

lv_obj_t *ui_home_get_screen(void)
{
    return s_home_screen;
}

void ui_home_update_wifi_status(bool connected, const char *ip)
{
    if (s_lbl_wifi_icon) {
        lv_obj_set_style_text_color(s_lbl_wifi_icon, 
            connected ? HARPY_COLOR_SUCCESS : HARPY_COLOR_MUTED, 0);
    }
    if (s_lbl_ip) {
        lv_label_set_text(s_lbl_ip, connected ? ip : "Not connected");
    }
}

void ui_home_update_bt_status(bool enabled)
{
    if (s_lbl_bt_icon) {
        lv_obj_set_style_text_color(s_lbl_bt_icon,
            enabled ? HARPY_COLOR_PRIMARY : HARPY_COLOR_MUTED, 0);
    }
}

void ui_home_update_time(int hour, int min)
{
    if (s_lbl_time) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d:%02d", hour, min);
        lv_label_set_text(s_lbl_time, buf);
    }
}
