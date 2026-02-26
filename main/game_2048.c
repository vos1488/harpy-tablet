/*
 * 2048 Game for HARPY Tablet
 *
 * Classic sliding-tile puzzle game.
 * Swipe to merge tiles. Reach 2048 to win!
 * 800x480 touch display, LVGL 8.3
 */

#include "ui_home.h"
#include "harpy_config.h"
#include "esp_log.h"
#include "esp_random.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "game_2048";

/* ==================== Game State ==================== */
#define GRID_SIZE  4
#define CELL_SIZE  90
#define CELL_GAP   8
#define BOARD_PAD  12
#define BOARD_SIZE (CELL_SIZE * GRID_SIZE + CELL_GAP * (GRID_SIZE - 1) + BOARD_PAD * 2)

static int      s_grid[GRID_SIZE][GRID_SIZE];
static int      s_score;
static bool     s_game_over;
static bool     s_won;

/* LVGL objects */
static lv_obj_t *s_screen       = NULL;
static lv_obj_t *s_board        = NULL;
static lv_obj_t *s_cells[GRID_SIZE][GRID_SIZE];
static lv_obj_t *s_score_lbl    = NULL;
static lv_obj_t *s_status_lbl   = NULL;

/* Swipe detection */
static lv_point_t s_press_point;
static bool        s_pressing = false;

/* ==================== Colors per tile value ==================== */
static lv_color_t tile_color(int val)
{
    switch (val) {
        case 2:    return lv_color_hex(0xEEE4DA);
        case 4:    return lv_color_hex(0xEDE0C8);
        case 8:    return lv_color_hex(0xF2B179);
        case 16:   return lv_color_hex(0xF59563);
        case 32:   return lv_color_hex(0xF67C5F);
        case 64:   return lv_color_hex(0xF65E3B);
        case 128:  return lv_color_hex(0xEDCF72);
        case 256:  return lv_color_hex(0xEDCC61);
        case 512:  return lv_color_hex(0xEDC850);
        case 1024: return lv_color_hex(0xEDC53F);
        case 2048: return lv_color_hex(0xEDC22E);
        default:   return lv_color_hex(0x3C3A32);
    }
}

static lv_color_t tile_text_color(int val)
{
    return val <= 4 ? lv_color_hex(0x776E65) : lv_color_white();
}

static const lv_font_t *tile_font(int val)
{
    if (val >= 1024) return &lv_font_montserrat_20;
    if (val >= 128)  return &lv_font_montserrat_24;
    return &lv_font_montserrat_28;
}

/* ==================== Game Logic ==================== */

static void add_random_tile(void)
{
    int empty[16][2];
    int count = 0;
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++)
            if (s_grid[r][c] == 0) {
                empty[count][0] = r;
                empty[count][1] = c;
                count++;
            }
    if (count == 0) return;

    int idx = esp_random() % count;
    s_grid[empty[idx][0]][empty[idx][1]] = (esp_random() % 10 < 9) ? 2 : 4;
}

static bool can_move(void)
{
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++) {
            if (s_grid[r][c] == 0) return true;
            if (c < GRID_SIZE - 1 && s_grid[r][c] == s_grid[r][c + 1]) return true;
            if (r < GRID_SIZE - 1 && s_grid[r][c] == s_grid[r + 1][c]) return true;
        }
    return false;
}

/* Slide & merge a single row left, return true if changed */
static bool slide_row(int row[GRID_SIZE])
{
    int tmp[GRID_SIZE] = {0};
    int pos = 0;
    bool changed = false;

    /* Compact non-zero */
    for (int i = 0; i < GRID_SIZE; i++)
        if (row[i]) tmp[pos++] = row[i];

    /* Merge adjacent equal */
    for (int i = 0; i < GRID_SIZE - 1; i++) {
        if (tmp[i] && tmp[i] == tmp[i + 1]) {
            tmp[i] *= 2;
            s_score += tmp[i];
            if (tmp[i] == 2048) s_won = true;
            tmp[i + 1] = 0;
        }
    }

    /* Re-compact */
    int out[GRID_SIZE] = {0};
    pos = 0;
    for (int i = 0; i < GRID_SIZE; i++)
        if (tmp[i]) out[pos++] = tmp[i];

    for (int i = 0; i < GRID_SIZE; i++) {
        if (row[i] != out[i]) changed = true;
        row[i] = out[i];
    }
    return changed;
}

typedef enum { DIR_LEFT, DIR_RIGHT, DIR_UP, DIR_DOWN } dir_t;

static bool do_move(dir_t dir)
{
    bool changed = false;
    int row[GRID_SIZE];

    for (int i = 0; i < GRID_SIZE; i++) {
        /* Extract row/column in canonical left-to-right order */
        switch (dir) {
            case DIR_LEFT:
                for (int j = 0; j < GRID_SIZE; j++) row[j] = s_grid[i][j];
                break;
            case DIR_RIGHT:
                for (int j = 0; j < GRID_SIZE; j++) row[j] = s_grid[i][GRID_SIZE - 1 - j];
                break;
            case DIR_UP:
                for (int j = 0; j < GRID_SIZE; j++) row[j] = s_grid[j][i];
                break;
            case DIR_DOWN:
                for (int j = 0; j < GRID_SIZE; j++) row[j] = s_grid[GRID_SIZE - 1 - j][i];
                break;
        }

        if (slide_row(row)) changed = true;

        /* Put back */
        switch (dir) {
            case DIR_LEFT:
                for (int j = 0; j < GRID_SIZE; j++) s_grid[i][j] = row[j];
                break;
            case DIR_RIGHT:
                for (int j = 0; j < GRID_SIZE; j++) s_grid[i][GRID_SIZE - 1 - j] = row[j];
                break;
            case DIR_UP:
                for (int j = 0; j < GRID_SIZE; j++) s_grid[j][i] = row[j];
                break;
            case DIR_DOWN:
                for (int j = 0; j < GRID_SIZE; j++) s_grid[GRID_SIZE - 1 - j][i] = row[j];
                break;
        }
    }

    return changed;
}

/* ==================== UI Update ==================== */

static void update_board_ui(void)
{
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            lv_obj_t *cell = s_cells[r][c];
            int val = s_grid[r][c];

            if (val == 0) {
                lv_obj_set_style_bg_color(cell, lv_color_hex(0x1C2333), 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_60, 0);
                lv_obj_t *lbl = lv_obj_get_child(cell, 0);
                if (lbl) lv_label_set_text(lbl, "");
            } else {
                lv_obj_set_style_bg_color(cell, tile_color(val), 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
                lv_obj_t *lbl = lv_obj_get_child(cell, 0);
                if (lbl) {
                    char txt[8];
                    snprintf(txt, sizeof(txt), "%d", val);
                    lv_label_set_text(lbl, txt);
                    lv_obj_set_style_text_color(lbl, tile_text_color(val), 0);
                    lv_obj_set_style_text_font(lbl, tile_font(val), 0);
                }
            }
        }
    }

    /* Update score */
    if (s_score_lbl) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Score: %d", s_score);
        lv_label_set_text(s_score_lbl, buf);
    }

    /* Update status */
    if (s_status_lbl) {
        if (s_won) {
            lv_label_set_text(s_status_lbl, "You WIN! " LV_SYMBOL_OK);
            lv_obj_set_style_text_color(s_status_lbl, HARPY_COLOR_SUCCESS, 0);
        } else if (s_game_over) {
            lv_label_set_text(s_status_lbl, "Game Over!");
            lv_obj_set_style_text_color(s_status_lbl, HARPY_COLOR_ERROR, 0);
        } else {
            lv_label_set_text(s_status_lbl, "Swipe to play");
            lv_obj_set_style_text_color(s_status_lbl, HARPY_COLOR_MUTED, 0);
        }
    }
}

static void init_game(void)
{
    memset(s_grid, 0, sizeof(s_grid));
    s_score = 0;
    s_game_over = false;
    s_won = false;
    add_random_tile();
    add_random_tile();
}

/* ==================== Touch / Swipe Handler ==================== */

static void board_press_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_indev_get_point(indev, &s_press_point);
    s_pressing = true;
}

static void board_release_cb(lv_event_t *e)
{
    if (!s_pressing) return;
    s_pressing = false;

    if (s_game_over) return;

    lv_point_t rel;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_indev_get_point(indev, &rel);

    int dx = rel.x - s_press_point.x;
    int dy = rel.y - s_press_point.y;

    /* Need minimum 30px swipe */
    if (LV_ABS(dx) < 30 && LV_ABS(dy) < 30) return;

    dir_t dir;
    if (LV_ABS(dx) > LV_ABS(dy)) {
        dir = dx > 0 ? DIR_RIGHT : DIR_LEFT;
    } else {
        dir = dy > 0 ? DIR_DOWN : DIR_UP;
    }

    bool moved = do_move(dir);
    if (moved) {
        add_random_tile();
        if (!can_move()) s_game_over = true;
        update_board_ui();
    }
}

/* ==================== Screen Management ==================== */

static void back_cb(lv_event_t *e)
{
    lv_obj_t *scr = lv_event_get_user_data(e);
    lv_scr_load(ui_home_get_screen());
    if (scr) lv_obj_del(scr);
    s_screen = NULL;
    s_board = NULL;
    memset(s_cells, 0, sizeof(s_cells));
    s_score_lbl = NULL;
    s_status_lbl = NULL;
}

static void restart_cb(lv_event_t *e)
{
    init_game();
    update_board_ui();
}

void game_2048_open(void)
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
    lv_label_set_text(title, "2048");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xEDC22E), 0);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* Score */
    s_score_lbl = lv_label_create(top);
    lv_label_set_text(s_score_lbl, "Score: 0");
    lv_obj_set_style_text_font(s_score_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_score_lbl, HARPY_COLOR_TEXT, 0);
    lv_obj_align(s_score_lbl, LV_ALIGN_RIGHT_MID, -100, 0);

    /* Restart button */
    lv_obj_t *btn_rst = lv_btn_create(top);
    lv_obj_set_size(btn_rst, 36, 36);
    lv_obj_align(btn_rst, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_bg_color(btn_rst, HARPY_COLOR_SUCCESS, 0);
    lv_obj_set_style_radius(btn_rst, 10, 0);
    lv_obj_add_event_cb(btn_rst, restart_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(btn_rst);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(rl, lv_color_white(), 0);
    lv_obj_center(rl);

    /* Status label */
    s_status_lbl = lv_label_create(s_screen);
    lv_label_set_text(s_status_lbl, "Swipe to play");
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_status_lbl, HARPY_COLOR_MUTED, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_MID, 0, -15);

    /* ===== Game Board ===== */
    s_board = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_board);
    lv_obj_set_size(s_board, BOARD_SIZE, BOARD_SIZE);
    lv_obj_set_style_bg_color(s_board, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_bg_opa(s_board, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_board, 16, 0);
    lv_obj_set_style_pad_all(s_board, BOARD_PAD, 0);
    lv_obj_center(s_board);
    lv_obj_clear_flag(s_board, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_board, LV_OBJ_FLAG_CLICKABLE);

    /* Touch events for swipe detection */
    lv_obj_add_event_cb(s_board, board_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_board, board_release_cb, LV_EVENT_RELEASED, NULL);

    /* Create cells */
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            lv_obj_t *cell = lv_obj_create(s_board);
            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, CELL_SIZE, CELL_SIZE);
            lv_obj_set_pos(cell, c * (CELL_SIZE + CELL_GAP), r * (CELL_SIZE + CELL_GAP));
            lv_obj_set_style_bg_color(cell, lv_color_hex(0x1C2333), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_60, 0);
            lv_obj_set_style_radius(cell, 10, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *lbl = lv_label_create(cell);
            lv_label_set_text(lbl, "");
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
            lv_obj_center(lbl);

            s_cells[r][c] = cell;
        }
    }

    /* Init game and draw */
    init_game();
    update_board_ui();

    lv_scr_load(s_screen);
    ESP_LOGI(TAG, "2048 game opened");
}
