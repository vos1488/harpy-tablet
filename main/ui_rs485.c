/*
 * RS485 Terminal
 * UART2 serial terminal with configurable baud rate.
 * Send/receive data via on-board RS485 transceiver.
 */

#include "ui_rs485.h"
#include "ui_home.h"
#include "harpy_config.h"
#include "ui_keyboard.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "lvgl.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ui_485";

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_log_ta = NULL;
static lv_obj_t *s_baud_lbl = NULL;
static lv_timer_t *s_rx_timer = NULL;

static bool s_uart_inited = false;
static int s_baud_rate = RS485_DEFAULT_BAUD;
static uint8_t s_rx_buf[RS485_BUF_SIZE];

/* ==================== UART Init ==================== */

static void uart_init(void)
{
    if (s_uart_inited) {
        uart_driver_delete(RS485_UART_NUM);
    }

    uart_config_t cfg = {
        .baud_rate = s_baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(RS485_UART_NUM, RS485_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(RS485_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(RS485_UART_NUM, RS485_TX_PIN, RS485_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    s_uart_inited = true;
    ESP_LOGI(TAG, "UART%d init @ %d baud", RS485_UART_NUM, s_baud_rate);
}

/* ==================== RX Poll Timer ==================== */

static void rx_poll_cb(lv_timer_t *timer)
{
    if (!s_uart_inited || !s_log_ta) return;

    int len = uart_read_bytes(RS485_UART_NUM, s_rx_buf, sizeof(s_rx_buf) - 1,
                               0);  /* Non-blocking */
    if (len > 0) {
        s_rx_buf[len] = '\0';
        /* Append as hex + ascii */
        char line[RS485_BUF_SIZE * 4];
        int pos = 0;
        pos += snprintf(line + pos, sizeof(line) - pos, "[RX %d] ", len);
        for (int i = 0; i < len && pos < (int)sizeof(line) - 8; i++) {
            if (s_rx_buf[i] >= 0x20 && s_rx_buf[i] < 0x7F) {
                line[pos++] = s_rx_buf[i];
            } else {
                pos += snprintf(line + pos, sizeof(line) - pos, "\\x%02X", s_rx_buf[i]);
            }
        }
        line[pos] = '\0';
        lv_textarea_add_text(s_log_ta, line);
        lv_textarea_add_text(s_log_ta, "\n");
    }
}

/* ==================== Callbacks ==================== */

static void send_text_cb(const char *text)
{
    if (!s_uart_inited || !text || strlen(text) == 0) return;

    uart_write_bytes(RS485_UART_NUM, text, strlen(text));
    uart_write_bytes(RS485_UART_NUM, "\r\n", 2);

    if (s_log_ta) {
        char line[256];
        snprintf(line, sizeof(line), "[TX] %s\n", text);
        lv_textarea_add_text(s_log_ta, line);
    }
    ESP_LOGI(TAG, "Sent: %s", text);
}

static void send_btn_cb(lv_event_t *e)
{
    ui_keyboard_show(lv_scr_act(), "Send RS485 Data", NULL, send_text_cb);
}

static void clear_btn_cb(lv_event_t *e)
{
    if (s_log_ta) lv_textarea_set_text(s_log_ta, "");
}

static void baud_cycle_cb(lv_event_t *e)
{
    static const int bauds[] = {9600, 19200, 38400, 57600, 115200, 230400};
    static const int n_bauds = sizeof(bauds) / sizeof(bauds[0]);
    int idx = 0;
    for (int i = 0; i < n_bauds; i++) {
        if (bauds[i] == s_baud_rate) { idx = (i + 1) % n_bauds; break; }
    }
    s_baud_rate = bauds[idx];
    uart_init();
    if (s_baud_lbl) {
        char txt[32];
        snprintf(txt, sizeof(txt), "%d baud", s_baud_rate);
        lv_label_set_text(s_baud_lbl, txt);
    }
}

static void back_cb(lv_event_t *e)
{
    if (s_rx_timer) { lv_timer_del(s_rx_timer); s_rx_timer = NULL; }
    s_log_ta = NULL;
    s_baud_lbl = NULL;
    lv_obj_t *scr = lv_event_get_user_data(e);
    lv_scr_load(ui_home_get_screen());
    if (scr) lv_obj_del(scr);
    s_screen = NULL;
}

/* ==================== Public API ==================== */

void ui_rs485_open(void)
{
    if (!s_uart_inited) uart_init();

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
    lv_label_set_text(title, LV_SYMBOL_CALL "  RS485 Terminal");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, HARPY_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* Content */
    lv_obj_t *content = lv_obj_create(s_screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, LCD_H_RES, LCD_V_RES - 50);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(content, 10, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 8, 0);

    /* Button bar */
    lv_obj_t *bar = lv_obj_create(content);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), 44);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 10, 0);

    /* Send button */
    lv_obj_t *send_btn = lv_btn_create(bar);
    lv_obj_set_style_bg_color(send_btn, HARPY_COLOR_SUCCESS, 0);
    lv_obj_set_style_radius(send_btn, 10, 0);
    lv_obj_set_style_pad_hor(send_btn, 16, 0);
    lv_obj_set_style_pad_ver(send_btn, 10, 0);
    lv_obj_add_event_cb(send_btn, send_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(send_btn);
    lv_label_set_text(sl, LV_SYMBOL_UPLOAD " Send");
    lv_obj_set_style_text_color(sl, lv_color_white(), 0);
    lv_obj_center(sl);

    /* Clear button */
    lv_obj_t *clr_btn = lv_btn_create(bar);
    lv_obj_set_style_bg_color(clr_btn, HARPY_COLOR_ERROR, 0);
    lv_obj_set_style_radius(clr_btn, 10, 0);
    lv_obj_set_style_pad_hor(clr_btn, 16, 0);
    lv_obj_set_style_pad_ver(clr_btn, 10, 0);
    lv_obj_add_event_cb(clr_btn, clear_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(clr_btn);
    lv_label_set_text(cl, LV_SYMBOL_TRASH " Clear");
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_obj_center(cl);

    /* Baud rate button */
    lv_obj_t *baud_btn = lv_btn_create(bar);
    lv_obj_set_style_bg_color(baud_btn, HARPY_COLOR_PRIMARY, 0);
    lv_obj_set_style_radius(baud_btn, 10, 0);
    lv_obj_set_style_pad_hor(baud_btn, 16, 0);
    lv_obj_set_style_pad_ver(baud_btn, 10, 0);
    lv_obj_add_event_cb(baud_btn, baud_cycle_cb, LV_EVENT_CLICKED, NULL);
    s_baud_lbl = lv_label_create(baud_btn);
    char baud_txt[32];
    snprintf(baud_txt, sizeof(baud_txt), "%d baud", s_baud_rate);
    lv_label_set_text(s_baud_lbl, baud_txt);
    lv_obj_set_style_text_color(s_baud_lbl, lv_color_white(), 0);
    lv_obj_center(s_baud_lbl);

    /* Log text area */
    s_log_ta = lv_textarea_create(content);
    lv_obj_set_size(s_log_ta, lv_pct(100), LCD_V_RES - 160);
    lv_textarea_set_text(s_log_ta, "RS485 Terminal ready\n");
    lv_obj_set_style_bg_color(s_log_ta, lv_color_hex(0x0A0E14), 0);
    lv_obj_set_style_text_color(s_log_ta, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(s_log_ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_border_color(s_log_ta, HARPY_COLOR_MUTED, 0);
    lv_obj_set_style_border_width(s_log_ta, 1, 0);
    lv_obj_set_style_radius(s_log_ta, 8, 0);
    lv_textarea_set_cursor_click_pos(s_log_ta, false);

    /* Start RX poll timer (50ms) */
    s_rx_timer = lv_timer_create(rx_poll_cb, 50, NULL);

    lv_scr_load(s_screen);
    ESP_LOGI(TAG, "RS485 terminal opened");
}
