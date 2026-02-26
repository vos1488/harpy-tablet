/*
 * ADC Sensor Monitor
 * Live voltage meter using ADC1 Channel 5 (GPIO 6).
 * Rolling chart + statistics (min/max/avg).
 */

#include "ui_adc.h"
#include "ui_home.h"
#include "harpy_config.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "lvgl.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "ui_adc";

#define ADC_CHART_POINTS  200
#define ADC_POLL_MS       100

static lv_obj_t  *s_screen   = NULL;
static lv_obj_t  *s_chart    = NULL;
static lv_chart_series_t *s_series = NULL;
static lv_obj_t  *s_volt_lbl = NULL;
static lv_obj_t  *s_raw_lbl  = NULL;
static lv_obj_t  *s_min_lbl  = NULL;
static lv_obj_t  *s_max_lbl  = NULL;
static lv_obj_t  *s_avg_lbl  = NULL;
static lv_timer_t *s_timer   = NULL;

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t         s_cali_handle = NULL;

static int s_min_mv =  99999;
static int s_max_mv = -99999;
static int64_t s_sum_mv = 0;
static int s_n_samples  = 0;

/* ==================== ADC hardware ==================== */

static void adc_hw_init(void)
{
    if (s_adc_handle) return;

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_SENSOR_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, ADC_SENSOR_CHANNEL, &chan_cfg));

    /* Calibration — try curve-fitting first, then line-fitting */
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_SENSOR_UNIT,
        .chan = ADC_SENSOR_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
    if (ret == ESP_OK) { ESP_LOGI(TAG, "ADC curve-fitting cal OK"); return; }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t line_cfg = {
        .unit_id = ADC_SENSOR_UNIT,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t ret2 = adc_cali_create_scheme_line_fitting(&line_cfg, &s_cali_handle);
    if (ret2 == ESP_OK) { ESP_LOGI(TAG, "ADC line-fitting cal OK"); return; }
#endif
    ESP_LOGW(TAG, "No ADC calibration available, raw values only");
}

static void adc_hw_deinit(void)
{
    if (s_cali_handle) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_delete_scheme_curve_fitting(s_cali_handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        adc_cali_delete_scheme_line_fitting(s_cali_handle);
#endif
        s_cali_handle = NULL;
    }
    if (s_adc_handle) {
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
    }
}

/* ==================== Poll timer ==================== */

static void adc_poll_cb(lv_timer_t *timer)
{
    if (!s_adc_handle) return;

    int raw = 0;
    esp_err_t ret = adc_oneshot_read(s_adc_handle, ADC_SENSOR_CHANNEL, &raw);
    if (ret != ESP_OK) return;

    int mv = raw;  /* fallback: raw value */
    if (s_cali_handle) {
        adc_cali_raw_to_voltage(s_cali_handle, raw, &mv);
    }

    /* Update stats */
    if (mv < s_min_mv) s_min_mv = mv;
    if (mv > s_max_mv) s_max_mv = mv;
    s_sum_mv += mv;
    s_n_samples++;

    /* Update labels */
    if (s_volt_lbl) {
        char txt[32];
        snprintf(txt, sizeof(txt), "%d mV", mv);
        lv_label_set_text(s_volt_lbl, txt);
    }
    if (s_raw_lbl) {
        char txt[32];
        snprintf(txt, sizeof(txt), "Raw: %d", raw);
        lv_label_set_text(s_raw_lbl, txt);
    }
    if (s_min_lbl) {
        char txt[32];
        snprintf(txt, sizeof(txt), "Min: %d mV", s_min_mv);
        lv_label_set_text(s_min_lbl, txt);
    }
    if (s_max_lbl) {
        char txt[32];
        snprintf(txt, sizeof(txt), "Max: %d mV", s_max_mv);
        lv_label_set_text(s_max_lbl, txt);
    }
    if (s_avg_lbl && s_n_samples > 0) {
        char txt[32];
        snprintf(txt, sizeof(txt), "Avg: %lld mV", (long long)(s_sum_mv / s_n_samples));
        lv_label_set_text(s_avg_lbl, txt);
    }

    /* Update chart */
    if (s_chart && s_series) {
        lv_chart_set_next_value(s_chart, s_series, mv);
    }
}

/* ==================== Callbacks ==================== */

static void reset_stats_cb(lv_event_t *e)
{
    s_min_mv = 99999;
    s_max_mv = -99999;
    s_sum_mv = 0;
    s_n_samples = 0;
}

static void back_cb(lv_event_t *e)
{
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    adc_hw_deinit();
    s_chart = NULL;
    s_series = NULL;
    s_volt_lbl = NULL;
    s_raw_lbl = NULL;
    s_min_lbl = NULL;
    s_max_lbl = NULL;
    s_avg_lbl = NULL;
    lv_obj_t *scr = lv_event_get_user_data(e);
    lv_scr_load(ui_home_get_screen());
    if (scr) lv_obj_del(scr);
    s_screen = NULL;
}

/* ==================== Public API ==================== */

void ui_adc_open(void)
{
    adc_hw_init();

    s_min_mv = 99999;
    s_max_mv = -99999;
    s_sum_mv = 0;
    s_n_samples = 0;

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
    lv_label_set_text(title, LV_SYMBOL_CHARGE "  ADC Sensor Monitor");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, HARPY_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* Content area — two columns */
    lv_obj_t *content = lv_obj_create(s_screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, LCD_H_RES - 20, LCD_V_RES - 60);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(content, 10, 0);

    /* Left side: voltage + stats */
    lv_obj_t *left = lv_obj_create(content);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, 220, lv_pct(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 10, 0);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Big voltage display */
    s_volt_lbl = lv_label_create(left);
    lv_label_set_text(s_volt_lbl, "--- mV");
    lv_obj_set_style_text_font(s_volt_lbl, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(s_volt_lbl, HARPY_COLOR_ACCENT, 0);

    s_raw_lbl = lv_label_create(left);
    lv_label_set_text(s_raw_lbl, "Raw: ---");
    lv_obj_set_style_text_color(s_raw_lbl, HARPY_COLOR_MUTED, 0);

    /* Stats panel */
    lv_obj_t *stats = lv_obj_create(left);
    lv_obj_set_size(stats, 200, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(stats, HARPY_COLOR_CARD, 0);
    lv_obj_set_style_radius(stats, 10, 0);
    lv_obj_set_style_pad_all(stats, 10, 0);
    lv_obj_set_style_border_width(stats, 0, 0);
    lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(stats, 6, 0);

    lv_obj_t *stats_title = lv_label_create(stats);
    lv_label_set_text(stats_title, "Statistics");
    lv_obj_set_style_text_font(stats_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(stats_title, HARPY_COLOR_TEXT, 0);

    s_min_lbl = lv_label_create(stats);
    lv_label_set_text(s_min_lbl, "Min: --- mV");
    lv_obj_set_style_text_color(s_min_lbl, HARPY_COLOR_SUCCESS, 0);

    s_max_lbl = lv_label_create(stats);
    lv_label_set_text(s_max_lbl, "Max: --- mV");
    lv_obj_set_style_text_color(s_max_lbl, HARPY_COLOR_ERROR, 0);

    s_avg_lbl = lv_label_create(stats);
    lv_label_set_text(s_avg_lbl, "Avg: --- mV");
    lv_obj_set_style_text_color(s_avg_lbl, HARPY_COLOR_PRIMARY, 0);

    /* Reset button */
    lv_obj_t *rst_btn = lv_btn_create(left);
    lv_obj_set_style_bg_color(rst_btn, HARPY_COLOR_ERROR, 0);
    lv_obj_set_style_radius(rst_btn, 10, 0);
    lv_obj_set_style_pad_hor(rst_btn, 20, 0);
    lv_obj_set_style_pad_ver(rst_btn, 10, 0);
    lv_obj_add_event_cb(rst_btn, reset_stats_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(rst_btn);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH " Reset");
    lv_obj_set_style_text_color(rl, lv_color_white(), 0);
    lv_obj_center(rl);

    /* Right side: chart */
    lv_obj_t *right = lv_obj_create(content);
    lv_obj_set_size(right, LCD_H_RES - 260, lv_pct(100));
    lv_obj_set_style_bg_color(right, HARPY_COLOR_CARD, 0);
    lv_obj_set_style_radius(right, 12, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 10, 0);

    lv_obj_t *chart_title = lv_label_create(right);
    lv_label_set_text(chart_title, "Voltage (mV)");
    lv_obj_set_style_text_color(chart_title, HARPY_COLOR_TEXT, 0);
    lv_obj_align(chart_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_chart = lv_chart_create(right);
    lv_obj_set_size(s_chart, lv_pct(100), lv_pct(100));
    lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, ADC_CHART_POINTS);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 3300);
    lv_chart_set_div_line_count(s_chart, 5, 8);
    lv_obj_set_style_size(s_chart, 0, LV_PART_INDICATOR);   /* No point dots */
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(0x0A0E14), 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(0x1A2030), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_chart, 0, 0);

    s_series = lv_chart_add_series(s_chart, HARPY_COLOR_ACCENT,
                                    LV_CHART_AXIS_PRIMARY_Y);

    /* Init chart with zeros */
    for (int i = 0; i < ADC_CHART_POINTS; i++) {
        lv_chart_set_next_value(s_chart, s_series, 0);
    }

    /* Start sampling timer */
    s_timer = lv_timer_create(adc_poll_cb, ADC_POLL_MS, NULL);

    lv_scr_load(s_screen);
    ESP_LOGI(TAG, "ADC monitor opened (GPIO %d, CH%d)", ADC_SENSOR_GPIO, ADC_SENSOR_CHANNEL);
}
