/*
 * Snake Game for HARPY Tablet
 *
 * Classic snake game with touch controls.
 * Tap the quadrant of the screen to turn the snake.
 * 800x480 touch display, LVGL 8.3
 */

#include "ui_home.h"
#include "harpy_config.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "game_snake";

/* ==================== Game Constants ==================== */
#define COLS         32
#define ROWS         18
#define TILE         22
#define FIELD_W      (COLS * TILE)   /* 704 */
#define FIELD_H      (ROWS * TILE)   /* 396 */
#define MAX_SNAKE    (COLS * ROWS)
#define INIT_LEN     4
#define TICK_MS      120              /* ms per game step */

typedef enum { D_UP, D_DOWN, D_LEFT, D_RIGHT } snake_dir_t;

/* ==================== Game State ==================== */
static int      s_snake_x[MAX_SNAKE];
static int      s_snake_y[MAX_SNAKE];
static int      s_len;
static snake_dir_t s_dir;
static int      s_food_x, s_food_y;
static int      s_snake_score;
static bool     s_alive;
static bool     s_paused;

/* LVGL objects */
static lv_obj_t *s_screen     = NULL;
static lv_obj_t *s_canvas     = NULL;
static lv_obj_t *s_score_lbl  = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_timer_t *s_timer    = NULL;

/* Canvas buffer in PSRAM */
static lv_color_t *s_cbuf = NULL;

/* Colors */
#define C_BG     lv_color_hex(0x0D1117)
#define C_GRID   lv_color_hex(0x161B22)
#define C_SNAKE  lv_color_hex(0x3FB950)
#define C_HEAD   lv_color_hex(0x58A6FF)
#define C_FOOD   lv_color_hex(0xF85149)

/* ==================== Game Logic ==================== */

static void place_food(void)
{
    /* Find an empty cell */
    for (int attempt = 0; attempt < 200; attempt++) {
        int x = esp_random() % COLS;
        int y = esp_random() % ROWS;
        bool on_snake = false;
        for (int i = 0; i < s_len; i++) {
            if (s_snake_x[i] == x && s_snake_y[i] == y) {
                on_snake = true;
                break;
            }
        }
        if (!on_snake) {
            s_food_x = x;
            s_food_y = y;
            return;
        }
    }
}

static void init_snake(void)
{
    s_len = INIT_LEN;
    s_dir = D_RIGHT;
    s_snake_score = 0;
    s_alive = true;
    s_paused = false;
    for (int i = 0; i < s_len; i++) {
        s_snake_x[i] = COLS / 2 - i;
        s_snake_y[i] = ROWS / 2;
    }
    place_food();
}

static void step_snake(void)
{
    if (!s_alive || s_paused) return;

    /* Calculate new head */
    int nx = s_snake_x[0];
    int ny = s_snake_y[0];
    switch (s_dir) {
        case D_UP:    ny--; break;
        case D_DOWN:  ny++; break;
        case D_LEFT:  nx--; break;
        case D_RIGHT: nx++; break;
    }

    /* Wall collision */
    if (nx < 0 || nx >= COLS || ny < 0 || ny >= ROWS) {
        s_alive = false;
        return;
    }

    /* Self collision */
    for (int i = 0; i < s_len; i++) {
        if (s_snake_x[i] == nx && s_snake_y[i] == ny) {
            s_alive = false;
            return;
        }
    }

    /* Move body */
    bool ate = (nx == s_food_x && ny == s_food_y);
    if (!ate) {
        /* Shift tail out */
        for (int i = s_len - 1; i > 0; i--) {
            s_snake_x[i] = s_snake_x[i - 1];
            s_snake_y[i] = s_snake_y[i - 1];
        }
    } else {
        /* Grow: shift everything, keep old tail */
        if (s_len < MAX_SNAKE) s_len++;
        for (int i = s_len - 1; i > 0; i--) {
            s_snake_x[i] = s_snake_x[i - 1];
            s_snake_y[i] = s_snake_y[i - 1];
        }
        s_snake_score += 10;
        place_food();
    }

    s_snake_x[0] = nx;
    s_snake_y[0] = ny;
}

/* ==================== Rendering ==================== */

static void draw_field(void)
{
    if (!s_cbuf) return;

    lv_canvas_fill_bg(s_canvas, C_BG, LV_OPA_COVER);

    /* Draw grid lines */
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.border_width = 0;

    /* Food */
    rect_dsc.bg_color = C_FOOD;
    rect_dsc.bg_opa = LV_OPA_COVER;
    rect_dsc.radius = 6;
    lv_canvas_draw_rect(s_canvas, s_food_x * TILE + 2, s_food_y * TILE + 2,
                        TILE - 4, TILE - 4, &rect_dsc);

    /* Snake body */
    rect_dsc.radius = 4;
    for (int i = s_len - 1; i >= 0; i--) {
        rect_dsc.bg_color = (i == 0) ? C_HEAD : C_SNAKE;
        int pad = (i == 0) ? 1 : 2;
        lv_canvas_draw_rect(s_canvas, s_snake_x[i] * TILE + pad,
                            s_snake_y[i] * TILE + pad,
                            TILE - pad * 2, TILE - pad * 2, &rect_dsc);
    }

    lv_obj_invalidate(s_canvas);
}

/* ==================== Timer & UI ==================== */

static void game_timer_cb(lv_timer_t *timer)
{
    step_snake();
    draw_field();

    if (s_score_lbl) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Score: %d", s_snake_score);
        lv_label_set_text(s_score_lbl, buf);
    }

    if (s_status_lbl) {
        if (!s_alive) {
            lv_label_set_text(s_status_lbl, "Game Over! Tap to restart");
            lv_obj_set_style_text_color(s_status_lbl, HARPY_COLOR_ERROR, 0);
        } else {
            lv_label_set_text(s_status_lbl, "");
        }
    }
}

/* ==================== Touch Controls ==================== */
/*
 * Touch direction: tap in one of 4 quadrants of the field
 * to change direction. Cannot reverse (e.g. up->down).
 */
static void field_click_cb(lv_event_t *e)
{
    if (!s_alive) {
        init_snake();
        return;
    }

    lv_point_t p;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_indev_get_point(indev, &p);

    /* Get click position relative to canvas */
    lv_area_t a;
    lv_obj_get_coords(s_canvas, &a);
    int rx = p.x - a.x1;
    int ry = p.y - a.y1;

    /* Head position in pixels */
    int hx = s_snake_x[0] * TILE + TILE / 2;
    int hy = s_snake_y[0] * TILE + TILE / 2;

    int dx = rx - hx;
    int dy = ry - hy;

    snake_dir_t new_dir = s_dir;
    if (LV_ABS(dx) > LV_ABS(dy)) {
        new_dir = dx > 0 ? D_RIGHT : D_LEFT;
    } else {
        new_dir = dy > 0 ? D_DOWN : D_UP;
    }

    /* Prevent 180-degree turn */
    if ((s_dir == D_UP && new_dir == D_DOWN) ||
        (s_dir == D_DOWN && new_dir == D_UP) ||
        (s_dir == D_LEFT && new_dir == D_RIGHT) ||
        (s_dir == D_RIGHT && new_dir == D_LEFT)) {
        return;
    }

    s_dir = new_dir;
}

/* ==================== Screen Management ==================== */

static void back_cb(lv_event_t *e)
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    lv_obj_t *scr = lv_event_get_user_data(e);
    lv_scr_load(ui_home_get_screen());
    if (scr) lv_obj_del(scr);
    s_screen = NULL;
    s_canvas = NULL;
    s_score_lbl = NULL;
    s_status_lbl = NULL;
    if (s_cbuf) {
        heap_caps_free(s_cbuf);
        s_cbuf = NULL;
    }
}

void game_snake_open(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x0a0e17), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    /* Top bar */
    lv_obj_t *top = lv_obj_create(s_screen);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LCD_H_RES, 42);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_80, 0);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);

    /* Back button */
    lv_obj_t *btn_back = lv_btn_create(top);
    lv_obj_set_size(btn_back, 80, 36);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(btn_back, HARPY_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(btn_back, 10, 0);
    lv_obj_add_event_cb(btn_back, back_cb, LV_EVENT_CLICKED, s_screen);
    lv_obj_t *bl = lv_label_create(btn_back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_center(bl);

    /* Title */
    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, LV_SYMBOL_RIGHT " Snake");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, C_SNAKE, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* Score */
    s_score_lbl = lv_label_create(top);
    lv_label_set_text(s_score_lbl, "Score: 0");
    lv_obj_set_style_text_font(s_score_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_score_lbl, HARPY_COLOR_TEXT, 0);
    lv_obj_align(s_score_lbl, LV_ALIGN_RIGHT_MID, -12, 0);

    /* Canvas (game field) */
    s_cbuf = heap_caps_malloc(FIELD_W * FIELD_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!s_cbuf) {
        ESP_LOGE(TAG, "Failed to alloc canvas buffer");
        back_cb(NULL);
        return;
    }

    s_canvas = lv_canvas_create(s_screen);
    lv_canvas_set_buffer(s_canvas, s_cbuf, FIELD_W, FIELD_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 10);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, field_click_cb, LV_EVENT_CLICKED, NULL);

    /* Status label (below field) */
    s_status_lbl = lv_label_create(s_screen);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_status_lbl, HARPY_COLOR_ERROR, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* Init game */
    init_snake();
    draw_field();

    /* Game tick timer */
    s_timer = lv_timer_create(game_timer_cb, TICK_MS, NULL);

    lv_scr_load(s_screen);
    ESP_LOGI(TAG, "Snake game opened");
}
