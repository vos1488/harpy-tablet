/*
 * HARPY Boot Logo
 * Animated boot screen with HARPY text and effects
 */

#include "boot_logo.h"
#include "harpy_config.h"
#include "esp_log.h"

static const char *TAG = "boot_logo";

static void (*s_on_complete)(void) = NULL;
static lv_obj_t *s_boot_screen = NULL;

/* ==================== Animation Callbacks ==================== */

static void anim_opa_cb(void *obj, int32_t val)
{
    lv_obj_set_style_text_opa((lv_obj_t *)obj, val, 0);
}

static void anim_y_cb(void *obj, int32_t val)
{
    lv_obj_set_y((lv_obj_t *)obj, val);
}

static void anim_scale_cb(void *obj, int32_t val)
{
    lv_obj_set_style_transform_zoom((lv_obj_t *)obj, val, 0);
}

static void boot_complete_timer_cb(lv_timer_t *timer)
{
    ESP_LOGI(TAG, "Boot animation complete");
    lv_timer_del(timer);  /* Delete timer to prevent re-triggering */
    if (s_on_complete) {
        void (*cb)(void) = s_on_complete;
        s_on_complete = NULL;  /* Prevent double-call */
        cb();
    }
    if (s_boot_screen) {
        lv_obj_del(s_boot_screen);
        s_boot_screen = NULL;
    }
}

/* ==================== Particle Effect ==================== */

static void create_particles(lv_obj_t *parent, int cx, int cy)
{
    for (int i = 0; i < 12; i++) {
        lv_obj_t *dot = lv_obj_create(parent);
        lv_obj_remove_style_all(dot);
        
        int size = 4 + (i % 3) * 3;
        lv_obj_set_size(dot, size, size);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        
        /* Alternate colors between primary and accent */
        if (i % 2 == 0) {
            lv_obj_set_style_bg_color(dot, HARPY_COLOR_PRIMARY, 0);
        } else {
            lv_obj_set_style_bg_color(dot, HARPY_COLOR_ACCENT, 0);
        }

        /* Position around center */
        int angle = i * 30;
        int radius = 80 + (i % 3) * 30;
        int x = cx + (radius * lv_trigo_cos(angle * 10) / 32768) - size / 2;
        int y = cy + (radius * lv_trigo_sin(angle * 10) / 32768) - size / 2;
        lv_obj_set_pos(dot, x, y);
        lv_obj_set_style_opa(dot, LV_OPA_TRANSP, 0);

        /* Fade in animation */
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, dot);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_80);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
        lv_anim_set_time(&a, 400);
        lv_anim_set_delay(&a, 800 + i * 60);
        lv_anim_set_playback_time(&a, 600);
        lv_anim_start(&a);
    }
}

/* ==================== Glowing Line ==================== */

static void create_glow_line(lv_obj_t *parent, int y_pos)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 0, 2);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(line, HARPY_COLOR_PRIMARY, 0);
    lv_obj_set_style_radius(line, 1, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, y_pos);

    /* Expand animation */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, line);
    lv_anim_set_values(&a, 0, 300);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_width);
    lv_anim_set_time(&a, 600);
    lv_anim_set_delay(&a, 1600);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* ==================== Public API ==================== */

void boot_logo_show(lv_obj_t *parent, void (*on_complete)(void))
{
    ESP_LOGI(TAG, "Showing HARPY boot logo");
    s_on_complete = on_complete;

    /* Create full-screen boot container */
    s_boot_screen = lv_obj_create(parent);
    lv_obj_remove_style_all(s_boot_screen);
    lv_obj_set_size(s_boot_screen, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_opa(s_boot_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_boot_screen, HARPY_COLOR_BG, 0);
    lv_obj_center(s_boot_screen);

    /* ==== Main "HARPY" text ==== */
    lv_obj_t *lbl_harpy = lv_label_create(s_boot_screen);
    lv_label_set_text(lbl_harpy, "HARPY");
    lv_obj_set_style_text_font(lbl_harpy, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_harpy, lv_color_white(), 0);
    lv_obj_set_style_text_letter_space(lbl_harpy, 12, 0);
    lv_obj_set_style_text_opa(lbl_harpy, LV_OPA_TRANSP, 0);
    lv_obj_align(lbl_harpy, LV_ALIGN_CENTER, 0, -30);
    lv_obj_set_style_transform_zoom(lbl_harpy, 200, 0); /* Start small */

    /* Scale up animation */
    lv_anim_t a_scale;
    lv_anim_init(&a_scale);
    lv_anim_set_var(&a_scale, lbl_harpy);
    lv_anim_set_values(&a_scale, 200, 256); /* 256 = 100% */
    lv_anim_set_exec_cb(&a_scale, anim_scale_cb);
    lv_anim_set_time(&a_scale, 800);
    lv_anim_set_delay(&a_scale, 200);
    lv_anim_set_path_cb(&a_scale, lv_anim_path_overshoot);
    lv_anim_start(&a_scale);

    /* Fade in animation */
    lv_anim_t a_fade;
    lv_anim_init(&a_fade);
    lv_anim_set_var(&a_fade, lbl_harpy);
    lv_anim_set_values(&a_fade, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a_fade, anim_opa_cb);
    lv_anim_set_time(&a_fade, 600);
    lv_anim_set_delay(&a_fade, 200);
    lv_anim_start(&a_fade);

    /* ==== Subtitle ==== */
    lv_obj_t *lbl_sub = lv_label_create(s_boot_screen);
    lv_label_set_text(lbl_sub, "T A B L E T   S Y S T E M");
    lv_obj_set_style_text_font(lbl_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_sub, HARPY_COLOR_MUTED, 0);
    lv_obj_set_style_text_letter_space(lbl_sub, 4, 0);
    lv_obj_set_style_text_opa(lbl_sub, LV_OPA_TRANSP, 0);
    lv_obj_align(lbl_sub, LV_ALIGN_CENTER, 0, 25);

    /* Slide up + fade in */
    lv_anim_t a_sub_y;
    lv_anim_init(&a_sub_y);
    lv_anim_set_var(&a_sub_y, lbl_sub);
    lv_anim_set_values(&a_sub_y, 260, 240 + 25);
    lv_anim_set_exec_cb(&a_sub_y, anim_y_cb);
    lv_anim_set_time(&a_sub_y, 500);
    lv_anim_set_delay(&a_sub_y, 700);
    lv_anim_set_path_cb(&a_sub_y, lv_anim_path_ease_out);
    lv_anim_start(&a_sub_y);

    lv_anim_t a_sub_opa;
    lv_anim_init(&a_sub_opa);
    lv_anim_set_var(&a_sub_opa, lbl_sub);
    lv_anim_set_values(&a_sub_opa, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a_sub_opa, anim_opa_cb);
    lv_anim_set_time(&a_sub_opa, 500);
    lv_anim_set_delay(&a_sub_opa, 700);
    lv_anim_start(&a_sub_opa);

    /* ==== Particles ==== */
    create_particles(s_boot_screen, LCD_H_RES / 2, LCD_V_RES / 2 - 30);

    /* ==== Glowing line under text ==== */
    create_glow_line(s_boot_screen, LCD_V_RES / 2 + 45);

    /* ==== Loading text ==== */
    lv_obj_t *lbl_loading = lv_label_create(s_boot_screen);
    lv_label_set_text(lbl_loading, "Initializing...");
    lv_obj_set_style_text_font(lbl_loading, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_loading, HARPY_COLOR_MUTED, 0);
    lv_obj_set_style_text_opa(lbl_loading, LV_OPA_TRANSP, 0);
    lv_obj_align(lbl_loading, LV_ALIGN_BOTTOM_MID, 0, -40);

    lv_anim_t a_load;
    lv_anim_init(&a_load);
    lv_anim_set_var(&a_load, lbl_loading);
    lv_anim_set_values(&a_load, LV_OPA_TRANSP, LV_OPA_70);
    lv_anim_set_exec_cb(&a_load, anim_opa_cb);
    lv_anim_set_time(&a_load, 500);
    lv_anim_set_delay(&a_load, 1400);
    lv_anim_start(&a_load);

    /* ==== Version ==== */
    lv_obj_t *lbl_ver = lv_label_create(s_boot_screen);
    lv_label_set_text(lbl_ver, "v1.0.0");
    lv_obj_set_style_text_font(lbl_ver, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_ver, HARPY_COLOR_MUTED, 0);
    lv_obj_set_style_text_opa(lbl_ver, LV_OPA_50, 0);
    lv_obj_align(lbl_ver, LV_ALIGN_BOTTOM_RIGHT, -20, -15);

    /* ==== Complete timer - shows boot logo for ~3.5 seconds ==== */
    lv_timer_create(boot_complete_timer_cb, 3500, NULL);
}
