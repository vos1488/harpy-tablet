/*
 * Settings Screen
 * System information, display brightness, reboot, about.
 */

#include "ui_settings.h"
#include "ui_home.h"
#include "harpy_config.h"
#include "lcd_driver.h"
#include "wifi_manager.h"
#include "time_manager.h"

#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_idf_version.h"

#include <stdio.h>

static const char *TAG = "ui_set";

/* Forward declarations */
static lv_obj_t *s_settings_screen = NULL;

/* ==================== Back Button ==================== */

static void back_btn_cb(lv_event_t *e)
{
    lv_obj_t *scr = lv_event_get_user_data(e);
    lv_scr_load(ui_home_get_screen());
    if (scr) {
        lv_obj_del(scr);
        s_settings_screen = NULL;
    }
}

/* ==================== Brightness Slider ==================== */

static void brightness_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    lcd_driver_set_backlight(val);
}

/* ==================== Reboot Button ==================== */

static void reboot_btn_cb(lv_event_t *e)
{
    ESP_LOGW(TAG, "Rebooting...");
    esp_restart();
}

/* ==================== Helper: Info Row ==================== */

static lv_obj_t *create_info_row(lv_obj_t *parent, const char *label, const char *value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(row, 4, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, HARPY_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);

    lv_obj_t *val_lbl = lv_label_create(row);
    lv_label_set_text(val_lbl, value);
    lv_obj_set_style_text_color(val_lbl, HARPY_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_14, 0);

    return row;
}

/* ==================== Create Settings Screen ==================== */

void ui_settings_open(void)
{
    s_settings_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_settings_screen, HARPY_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_settings_screen, LV_OPA_COVER, 0);

    /* ===== Top Bar ===== */
    lv_obj_t *top = lv_obj_create(s_settings_screen);
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
    lv_obj_add_event_cb(btn_back, back_btn_cb, LV_EVENT_CLICKED, s_settings_screen);

    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(lbl_back, lv_color_white(), 0);
    lv_obj_center(lbl_back);

    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS "  Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, HARPY_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* ===== Content area (two columns) ===== */
    lv_obj_t *content = lv_obj_create(s_settings_screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, LCD_H_RES, LCD_V_RES - 50);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(content, 15, 0);
    lv_obj_set_style_pad_column(content, 15, 0);

    /* ===== LEFT COLUMN: Display & Controls ===== */
    lv_obj_t *left_col = lv_obj_create(content);
    lv_obj_remove_style_all(left_col);
    lv_obj_set_size(left_col, 370, lv_pct(100));
    lv_obj_set_style_bg_color(left_col, HARPY_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(left_col, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(left_col, 16, 0);
    lv_obj_set_style_pad_all(left_col, 20, 0);
    lv_obj_set_flex_flow(left_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left_col, 12, 0);

    /* Section: Display */
    lv_obj_t *disp_title = lv_label_create(left_col);
    lv_label_set_text(disp_title, "Display");
    lv_obj_set_style_text_font(disp_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(disp_title, HARPY_COLOR_PRIMARY, 0);

    /* Brightness label + slider */
    lv_obj_t *br_row = lv_obj_create(left_col);
    lv_obj_remove_style_all(br_row);
    lv_obj_set_size(br_row, lv_pct(100), 40);
    lv_obj_set_flex_flow(br_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(br_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(br_row, 12, 0);

    lv_obj_t *br_lbl = lv_label_create(br_row);
    lv_label_set_text(br_lbl, LV_SYMBOL_IMAGE "  Brightness");
    lv_obj_set_style_text_color(br_lbl, HARPY_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(br_lbl, &lv_font_montserrat_14, 0);

    lv_obj_t *slider = lv_slider_create(br_row);
    lv_obj_set_width(slider, 180);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, HARPY_COLOR_CARD, 0);
    lv_obj_set_style_bg_color(slider, HARPY_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, HARPY_COLOR_PRIMARY, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Divider */
    lv_obj_t *div1 = lv_obj_create(left_col);
    lv_obj_remove_style_all(div1);
    lv_obj_set_size(div1, lv_pct(100), 1);
    lv_obj_set_style_bg_color(div1, HARPY_COLOR_MUTED, 0);
    lv_obj_set_style_bg_opa(div1, LV_OPA_30, 0);

    /* Section: Actions */
    lv_obj_t *act_title = lv_label_create(left_col);
    lv_label_set_text(act_title, "Actions");
    lv_obj_set_style_text_font(act_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(act_title, HARPY_COLOR_PRIMARY, 0);

    lv_obj_t *reboot_btn = lv_btn_create(left_col);
    lv_obj_set_size(reboot_btn, 200, 44);
    lv_obj_set_style_bg_color(reboot_btn, HARPY_COLOR_ERROR, 0);
    lv_obj_set_style_radius(reboot_btn, 10, 0);
    lv_obj_add_event_cb(reboot_btn, reboot_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *reboot_lbl = lv_label_create(reboot_btn);
    lv_label_set_text(reboot_lbl, LV_SYMBOL_REFRESH "  Reboot");
    lv_obj_set_style_text_color(reboot_lbl, lv_color_white(), 0);
    lv_obj_center(reboot_lbl);

    /* ===== RIGHT COLUMN: System Info ===== */
    lv_obj_t *right_col = lv_obj_create(content);
    lv_obj_remove_style_all(right_col);
    lv_obj_set_size(right_col, 370, lv_pct(100));
    lv_obj_set_style_bg_color(right_col, HARPY_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(right_col, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(right_col, 16, 0);
    lv_obj_set_style_pad_all(right_col, 20, 0);
    lv_obj_set_flex_flow(right_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right_col, 6, 0);

    lv_obj_t *info_title = lv_label_create(right_col);
    lv_label_set_text(info_title, "System Info");
    lv_obj_set_style_text_font(info_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(info_title, HARPY_COLOR_PRIMARY, 0);

    /* Gather system info */
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    char buf[64];

    create_info_row(right_col, "Firmware", "HARPY v1.0");

    snprintf(buf, sizeof(buf), "ESP-IDF %s", esp_get_idf_version());
    create_info_row(right_col, "SDK", buf);

    snprintf(buf, sizeof(buf), "ESP32-S3 rev %d.%d (%d cores)",
             chip.revision / 100, chip.revision % 100, chip.cores);
    create_info_row(right_col, "Chip", buf);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    snprintf(buf, sizeof(buf), "%lu MB", (unsigned long)(flash_size / (1024 * 1024)));
    create_info_row(right_col, "Flash", buf);

    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    snprintf(buf, sizeof(buf), "%.1f MB", psram_size / (1024.0f * 1024.0f));
    create_info_row(right_col, "PSRAM", buf);

    /* Divider */
    lv_obj_t *div2 = lv_obj_create(right_col);
    lv_obj_remove_style_all(div2);
    lv_obj_set_size(div2, lv_pct(100), 1);
    lv_obj_set_style_bg_color(div2, HARPY_COLOR_MUTED, 0);
    lv_obj_set_style_bg_opa(div2, LV_OPA_30, 0);

    /* Memory section */
    lv_obj_t *mem_title = lv_label_create(right_col);
    lv_label_set_text(mem_title, "Memory");
    lv_obj_set_style_text_font(mem_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(mem_title, HARPY_COLOR_PRIMARY, 0);

    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snprintf(buf, sizeof(buf), "%u KB", (unsigned)(free_internal / 1024));
    create_info_row(right_col, "Free SRAM", buf);

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snprintf(buf, sizeof(buf), "%.1f MB", free_psram / (1024.0f * 1024.0f));
    create_info_row(right_col, "Free PSRAM", buf);

    size_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    snprintf(buf, sizeof(buf), "%u KB", (unsigned)(min_free / 1024));
    create_info_row(right_col, "Min SRAM", buf);

    /* WiFi info */
    wifi_state_t ws = wifi_manager_get_state();
    create_info_row(right_col, "WiFi",
        ws == WIFI_STATE_CONNECTED ? "Connected" :
        ws == WIFI_STATE_CONNECTING ? "Connecting" : "Disconnected");

    if (ws == WIFI_STATE_CONNECTED) {
        create_info_row(right_col, "IP", wifi_manager_get_ip());
    }

    lv_scr_load(s_settings_screen);
    ESP_LOGI(TAG, "Settings screen opened");
}
