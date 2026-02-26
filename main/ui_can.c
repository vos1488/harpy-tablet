/*
 * CAN Bus Monitor
 * TWAI (CAN 2.0) interface via GPIO 19 (RX) / GPIO 20 (TX).
 * Requires CH422G USB_SEL HIGH (already set in boot init).
 * Send/receive CAN frames with configurable speed.
 */

#include "ui_can.h"
#include "ui_home.h"
#include "harpy_config.h"
#include "ch422g.h"
#include "esp_log.h"
#include "driver/twai.h"

#include "lvgl.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ui_can";

static lv_obj_t  *s_screen  = NULL;
static lv_obj_t  *s_log_ta  = NULL;
static lv_obj_t  *s_id_ta   = NULL;
static lv_obj_t  *s_data_ta = NULL;
static lv_obj_t  *s_speed_lbl = NULL;
static lv_obj_t  *s_status_lbl = NULL;
static lv_timer_t *s_rx_timer = NULL;

static bool s_twai_running = false;
static int  s_speed_idx = 2; /* default 500 kbit */

typedef struct {
    const char *name;
    twai_timing_config_t timing;
} can_speed_t;

static const can_speed_t s_speeds[] = {
    {"25 kbit",  TWAI_TIMING_CONFIG_25KBITS()},
    {"125 kbit", TWAI_TIMING_CONFIG_125KBITS()},
    {"500 kbit", TWAI_TIMING_CONFIG_500KBITS()},
    {"1 Mbit",   TWAI_TIMING_CONFIG_1MBITS()},
};
#define N_SPEEDS (sizeof(s_speeds) / sizeof(s_speeds[0]))

/* ==================== TWAI driver ==================== */

static void can_driver_stop(void)
{
    if (s_twai_running) {
        twai_stop();
        twai_driver_uninstall();
        s_twai_running = false;
        ESP_LOGI(TAG, "TWAI stopped");
    }
}

static void can_driver_start(void)
{
    can_driver_stop();

    /* Ensure USB_SEL is HIGH (CAN mode) */
    ch422g_set_pins(I2C_BUS_NUM, CH422G_PIN_USB_SEL);

    twai_general_config_t g_cfg = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN,
                                                               TWAI_MODE_NORMAL);
    g_cfg.rx_queue_len = 16;
    g_cfg.tx_queue_len = 5;

    twai_timing_config_t t_cfg = s_speeds[s_speed_idx].timing;
    twai_filter_config_t f_cfg = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t ret = twai_driver_install(&g_cfg, &t_cfg, &f_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TWAI install fail: %s", esp_err_to_name(ret));
        return;
    }
    ret = twai_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TWAI start fail: %s", esp_err_to_name(ret));
        twai_driver_uninstall();
        return;
    }
    s_twai_running = true;
    ESP_LOGI(TAG, "TWAI started @ %s", s_speeds[s_speed_idx].name);
}

/* ==================== RX Poll ==================== */

static void rx_poll_cb(lv_timer_t *timer)
{
    if (!s_twai_running || !s_log_ta) return;

    twai_message_t msg;
    while (twai_receive(&msg, 0) == ESP_OK) {
        char line[128];
        int pos = 0;
        pos += snprintf(line + pos, sizeof(line) - pos,
                         "[RX] ID:0x%03lX DLC:%d ",
                         (unsigned long)msg.identifier, msg.data_length_code);
        for (int i = 0; i < msg.data_length_code && i < 8; i++) {
            pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", msg.data[i]);
        }
        if (msg.extd) {
            pos += snprintf(line + pos, sizeof(line) - pos, "(EXT)");
        }
        if (msg.rtr) {
            pos += snprintf(line + pos, sizeof(line) - pos, "(RTR)");
        }
        lv_textarea_add_text(s_log_ta, line);
        lv_textarea_add_text(s_log_ta, "\n");
    }

    /* Update status */
    if (s_status_lbl) {
        twai_status_info_t status;
        if (twai_get_status_info(&status) == ESP_OK) {
            char txt[64];
            snprintf(txt, sizeof(txt), "TX:%lu RX:%lu Err:%lu",
                     (unsigned long)status.msgs_to_tx,
                     (unsigned long)status.msgs_to_rx,
                     (unsigned long)status.bus_error_count);
            lv_label_set_text(s_status_lbl, txt);
        }
    }
}

/* ==================== Callbacks ==================== */

static void send_btn_cb(lv_event_t *e)
{
    if (!s_twai_running || !s_id_ta || !s_data_ta) return;

    const char *id_str = lv_textarea_get_text(s_id_ta);
    const char *data_str = lv_textarea_get_text(s_data_ta);

    uint32_t can_id = 0;
    sscanf(id_str, "%lx", (unsigned long *)&can_id);

    twai_message_t msg = {
        .identifier = can_id,
        .data_length_code = 0,
        .extd = (can_id > 0x7FF) ? 1 : 0,
    };

    /* Parse hex data bytes (space-separated) */
    const char *p = data_str;
    while (*p && msg.data_length_code < 8) {
        unsigned int byte_val = 0;
        if (sscanf(p, "%x", &byte_val) == 1) {
            msg.data[msg.data_length_code++] = byte_val & 0xFF;
        }
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }

    esp_err_t ret = twai_transmit(&msg, pdMS_TO_TICKS(100));
    char line[128];
    if (ret == ESP_OK) {
        int pos = snprintf(line, sizeof(line), "[TX] ID:0x%03lX DLC:%d ",
                           (unsigned long)msg.identifier, msg.data_length_code);
        for (int i = 0; i < msg.data_length_code; i++) {
            pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", msg.data[i]);
        }
        strcat(line, "\n");
    } else {
        snprintf(line, sizeof(line), "[TX FAIL] %s\n", esp_err_to_name(ret));
    }
    if (s_log_ta) lv_textarea_add_text(s_log_ta, line);
}

static void clear_btn_cb(lv_event_t *e)
{
    if (s_log_ta) lv_textarea_set_text(s_log_ta, "");
}

static void speed_cycle_cb(lv_event_t *e)
{
    s_speed_idx = (s_speed_idx + 1) % N_SPEEDS;
    can_driver_start();
    if (s_speed_lbl) lv_label_set_text(s_speed_lbl, s_speeds[s_speed_idx].name);
}

static void back_cb(lv_event_t *e)
{
    if (s_rx_timer) { lv_timer_del(s_rx_timer); s_rx_timer = NULL; }
    can_driver_stop();
    s_log_ta = NULL;
    s_id_ta = NULL;
    s_data_ta = NULL;
    s_speed_lbl = NULL;
    s_status_lbl = NULL;
    lv_obj_t *scr = lv_event_get_user_data(e);
    lv_scr_load(ui_home_get_screen());
    if (scr) lv_obj_del(scr);
    s_screen = NULL;
}

/* ==================== Public API ==================== */

void ui_can_open(void)
{
    can_driver_start();

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, HARPY_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    /* Top bar */
    lv_obj_t *top = lv_obj_create(s_screen);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LCD_H_RES, 50);
    lv_obj_set_style_bg_color(top, HARPY_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *btn_back = lv_btn_create(top);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_bg_color(btn_back, HARPY_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(btn_back, 8, 0);
    lv_obj_add_event_cb(btn_back, back_cb, LV_EVENT_CLICKED, s_screen);
    lv_obj_t *lb = lv_label_create(btn_back);
    lv_label_set_text(lb, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(lb, lv_color_white(), 0);
    lv_obj_center(lb);

    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, LV_SYMBOL_LOOP "  CAN Bus Monitor");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, HARPY_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* Content area */
    lv_obj_t *content = lv_obj_create(s_screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, LCD_H_RES - 20, LCD_V_RES - 60);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 6, 0);

    /* Control bar */
    lv_obj_t *ctrl = lv_obj_create(content);
    lv_obj_remove_style_all(ctrl);
    lv_obj_set_size(ctrl, lv_pct(100), 44);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctrl, 8, 0);

    /* ID input */
    lv_obj_t *id_lbl = lv_label_create(ctrl);
    lv_label_set_text(id_lbl, "ID:");
    lv_obj_set_style_text_color(id_lbl, HARPY_COLOR_TEXT, 0);

    s_id_ta = lv_textarea_create(ctrl);
    lv_obj_set_size(s_id_ta, 90, 38);
    lv_textarea_set_text(s_id_ta, "123");
    lv_textarea_set_one_line(s_id_ta, true);
    lv_textarea_set_max_length(s_id_ta, 8);
    lv_obj_set_style_bg_color(s_id_ta, HARPY_COLOR_CARD, 0);
    lv_obj_set_style_text_color(s_id_ta, HARPY_COLOR_TEXT, 0);
    lv_obj_set_style_border_color(s_id_ta, HARPY_COLOR_MUTED, 0);
    lv_obj_set_style_radius(s_id_ta, 6, 0);

    /* Data input */
    lv_obj_t *data_lbl = lv_label_create(ctrl);
    lv_label_set_text(data_lbl, "Data:");
    lv_obj_set_style_text_color(data_lbl, HARPY_COLOR_TEXT, 0);

    s_data_ta = lv_textarea_create(ctrl);
    lv_obj_set_size(s_data_ta, 200, 38);
    lv_textarea_set_text(s_data_ta, "01 02 03 04");
    lv_textarea_set_one_line(s_data_ta, true);
    lv_textarea_set_max_length(s_data_ta, 24);
    lv_obj_set_style_bg_color(s_data_ta, HARPY_COLOR_CARD, 0);
    lv_obj_set_style_text_color(s_data_ta, HARPY_COLOR_TEXT, 0);
    lv_obj_set_style_border_color(s_data_ta, HARPY_COLOR_MUTED, 0);
    lv_obj_set_style_radius(s_data_ta, 6, 0);

    /* Send button */
    lv_obj_t *send_btn = lv_btn_create(ctrl);
    lv_obj_set_style_bg_color(send_btn, HARPY_COLOR_SUCCESS, 0);
    lv_obj_set_style_radius(send_btn, 8, 0);
    lv_obj_set_style_pad_hor(send_btn, 14, 0);
    lv_obj_set_style_pad_ver(send_btn, 8, 0);
    lv_obj_add_event_cb(send_btn, send_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(send_btn);
    lv_label_set_text(sl, LV_SYMBOL_UPLOAD " TX");
    lv_obj_set_style_text_color(sl, lv_color_white(), 0);
    lv_obj_center(sl);

    /* Clear button */
    lv_obj_t *clr_btn = lv_btn_create(ctrl);
    lv_obj_set_style_bg_color(clr_btn, HARPY_COLOR_ERROR, 0);
    lv_obj_set_style_radius(clr_btn, 8, 0);
    lv_obj_set_style_pad_hor(clr_btn, 14, 0);
    lv_obj_set_style_pad_ver(clr_btn, 8, 0);
    lv_obj_add_event_cb(clr_btn, clear_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(clr_btn);
    lv_label_set_text(cl, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_obj_center(cl);

    /* Speed button */
    lv_obj_t *spd_btn = lv_btn_create(ctrl);
    lv_obj_set_style_bg_color(spd_btn, HARPY_COLOR_PRIMARY, 0);
    lv_obj_set_style_radius(spd_btn, 8, 0);
    lv_obj_set_style_pad_hor(spd_btn, 14, 0);
    lv_obj_set_style_pad_ver(spd_btn, 8, 0);
    lv_obj_add_event_cb(spd_btn, speed_cycle_cb, LV_EVENT_CLICKED, NULL);
    s_speed_lbl = lv_label_create(spd_btn);
    lv_label_set_text(s_speed_lbl, s_speeds[s_speed_idx].name);
    lv_obj_set_style_text_color(s_speed_lbl, lv_color_white(), 0);
    lv_obj_center(s_speed_lbl);

    /* Status line */
    s_status_lbl = lv_label_create(content);
    lv_label_set_text(s_status_lbl, "TX:0 RX:0 Err:0");
    lv_obj_set_style_text_color(s_status_lbl, HARPY_COLOR_MUTED, 0);

    /* Log text area */
    s_log_ta = lv_textarea_create(content);
    lv_obj_set_width(s_log_ta, lv_pct(100));
    lv_obj_set_flex_grow(s_log_ta, 1);
    lv_textarea_set_text(s_log_ta, "CAN Bus Monitor ready\n");
    lv_obj_set_style_bg_color(s_log_ta, lv_color_hex(0x0A0E14), 0);
    lv_obj_set_style_text_color(s_log_ta, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(s_log_ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_border_color(s_log_ta, HARPY_COLOR_MUTED, 0);
    lv_obj_set_style_border_width(s_log_ta, 1, 0);
    lv_obj_set_style_radius(s_log_ta, 8, 0);
    lv_textarea_set_cursor_click_pos(s_log_ta, false);

    /* Start RX poll timer */
    s_rx_timer = lv_timer_create(rx_poll_cb, 50, NULL);

    lv_scr_load(s_screen);
    ESP_LOGI(TAG, "CAN monitor opened (TX:%d RX:%d)", CAN_TX_PIN, CAN_RX_PIN);
}
