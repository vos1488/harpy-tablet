/*
 * On-Screen Keyboard for text input
 */

#include "ui_keyboard.h"
#include "harpy_config.h"
#include "esp_log.h"

static const char *TAG = "keyboard";

static lv_obj_t *s_kb_container = NULL;
static lv_obj_t *s_kb = NULL;
static lv_obj_t *s_ta = NULL;
static keyboard_done_cb_t s_done_cb = NULL;

/* ==================== Keyboard Event Handlers ==================== */

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *kb = lv_event_get_target(e);

    if (code == LV_EVENT_READY) {
        /* User pressed Enter/OK */
        const char *text = lv_textarea_get_text(s_ta);
        ESP_LOGI(TAG, "Input: %s", text);
        if (s_done_cb) {
            s_done_cb(text);
        }
        ui_keyboard_hide();
    } else if (code == LV_EVENT_CANCEL) {
        ui_keyboard_hide();
    }
}

static void ta_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        if (s_kb) {
            lv_keyboard_set_textarea(s_kb, s_ta);
        }
    }
}

/* ==================== Public API ==================== */

void ui_keyboard_show(lv_obj_t *parent, const char *title, 
                       const char *initial, keyboard_done_cb_t callback)
{
    /* Remove existing if any */
    ui_keyboard_hide();
    
    s_done_cb = callback;

    /* Full-screen overlay container */
    s_kb_container = lv_obj_create(parent);
    lv_obj_remove_style_all(s_kb_container);
    lv_obj_set_size(s_kb_container, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_opa(s_kb_container, LV_OPA_90, 0);
    lv_obj_set_style_bg_color(s_kb_container, HARPY_COLOR_BG, 0);
    lv_obj_center(s_kb_container);

    /* Title label */
    lv_obj_t *lbl = lv_label_create(s_kb_container);
    lv_label_set_text(lbl, title ? title : "Input");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, HARPY_COLOR_TEXT, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 15);

    /* Text area */
    s_ta = lv_textarea_create(s_kb_container);
    lv_textarea_set_one_line(s_ta, true);
    lv_obj_set_width(s_ta, LCD_H_RES - 80);
    lv_obj_align(s_ta, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_color(s_ta, HARPY_COLOR_CARD, 0);
    lv_obj_set_style_text_color(s_ta, HARPY_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_ta, &lv_font_montserrat_20, 0);
    lv_obj_set_style_border_color(s_ta, HARPY_COLOR_PRIMARY, 0);
    lv_obj_set_style_border_width(s_ta, 2, 0);
    lv_obj_set_style_radius(s_ta, 8, 0);
    lv_obj_set_style_pad_all(s_ta, 10, 0);
    lv_obj_add_event_cb(s_ta, ta_event_cb, LV_EVENT_ALL, NULL);

    if (initial) {
        lv_textarea_set_text(s_ta, initial);
    }

    /* Keyboard */
    s_kb = lv_keyboard_create(s_kb_container);
    lv_keyboard_set_textarea(s_kb, s_ta);
    lv_obj_set_size(s_kb, LCD_H_RES, LCD_V_RES - 110);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_kb, HARPY_COLOR_CARD, 0);
    lv_obj_set_style_bg_color(s_kb, HARPY_COLOR_PRIMARY, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_ALL, NULL);
}

void ui_keyboard_hide(void)
{
    if (s_kb_container) {
        lv_obj_del(s_kb_container);
        s_kb_container = NULL;
        s_kb = NULL;
        s_ta = NULL;
        s_done_cb = NULL;
    }
}
