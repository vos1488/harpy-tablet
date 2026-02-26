/*
 * Games Launcher UI
 *
 * Presents available games as a card grid.
 * Opens the selected game in a new LVGL screen.
 */

#include "ui_games.h"
#include "ui_home.h"
#include "harpy_config.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ui_games";

/* Forward declarations for game openers */
extern void game_2048_open(void);
extern void game_snake_open(void);

/* ==================== Screen ==================== */
static lv_obj_t *s_screen = NULL;

static void back_cb(lv_event_t *e)
{
    lv_obj_t *scr = lv_event_get_user_data(e);
    lv_scr_load(ui_home_get_screen());
    if (scr) lv_obj_del(scr);
    s_screen = NULL;
}

static void open_2048_cb(lv_event_t *e) { game_2048_open(); }
static void open_snake_cb(lv_event_t *e) { game_snake_open(); }

/* ==================== Game Card ==================== */

static lv_obj_t *create_game_card(lv_obj_t *parent,
                                   const char *icon,
                                   const char *name,
                                   const char *desc,
                                   lv_color_t color,
                                   lv_event_cb_t cb)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 220, 180);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2D3748), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_40, 0);
    lv_obj_set_style_shadow_width(card, 20, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
    lv_obj_set_style_shadow_color(card, color, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(card, LV_OPA_70, LV_STATE_PRESSED);

    /* Icon */
    lv_obj_t *icon_bg = lv_obj_create(card);
    lv_obj_remove_style_all(icon_bg);
    lv_obj_set_size(icon_bg, 60, 60);
    lv_obj_set_style_bg_color(icon_bg, color, 0);
    lv_obj_set_style_bg_opa(icon_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon_bg, 14, 0);
    lv_obj_clear_flag(icon_bg, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *icon_lbl = lv_label_create(icon_bg);
    lv_label_set_text(icon_lbl, icon);
    lv_obj_set_style_text_color(icon_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(icon_lbl);

    /* Name */
    lv_obj_t *name_lbl = lv_label_create(card);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_style_text_color(name_lbl, HARPY_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_20, 0);

    /* Description */
    lv_obj_t *desc_lbl = lv_label_create(card);
    lv_label_set_text(desc_lbl, desc);
    lv_obj_set_style_text_color(desc_lbl, HARPY_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(desc_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(desc_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(desc_lbl, 190);

    if (cb) lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);

    return card;
}

/* ==================== Public API ==================== */

void ui_games_open(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x0a0e17), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_color(s_screen, lv_color_hex(0x1a1040), 0);
    lv_obj_set_style_bg_grad_dir(s_screen, LV_GRAD_DIR_VER, 0);

    /* Top bar */
    lv_obj_t *top = lv_obj_create(s_screen);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LCD_H_RES, 50);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_80, 0);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);

    /* Back button */
    lv_obj_t *btn_back = lv_btn_create(top);
    lv_obj_set_size(btn_back, 80, 38);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_bg_color(btn_back, HARPY_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(btn_back, 10, 0);
    lv_obj_add_event_cb(btn_back, back_cb, LV_EVENT_CLICKED, s_screen);
    lv_obj_t *bl = lv_label_create(btn_back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_center(bl);

    /* Title */
    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, LV_SYMBOL_PLAY "  Games");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF97316), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* Game cards container */
    lv_obj_t *grid = lv_obj_create(s_screen);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LCD_H_RES, LCD_V_RES - 50);
    lv_obj_align(grid, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grid, 40, 0);
    lv_obj_set_style_pad_row(grid, 20, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    /* ===== Game Cards ===== */
    create_game_card(grid,
        LV_SYMBOL_LIST, "2048",
        "Slide tiles to merge",
        lv_color_hex(0xEDC22E),
        open_2048_cb);

    create_game_card(grid,
        LV_SYMBOL_RIGHT, "Snake",
        "Eat food, grow longer",
        lv_color_hex(0x3FB950),
        open_snake_cb);

    lv_scr_load(s_screen);
    ESP_LOGI(TAG, "Games launcher opened");
}
