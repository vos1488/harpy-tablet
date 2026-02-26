/*
 * LCD Driver for Waveshare ESP32-S3-Touch-LCD-4.3
 * 800x480 RGB LCD (ST7262) with LVGL integration
 *
 * Backlight and reset are controlled via CH422G I/O expander (I2C).
 * The ST7262 is a pure RGB panel — no SPI init commands required.
 *
 * Init sequence matches the official Waveshare demo exactly:
 *   1. Create and init RGB panel
 *   2. Init I2C bus
 *   3. Init GPIO4 as output
 *   4. CH422G touch reset sequence (also enables backlight)
 */

#include "lcd_driver.h"
#include "harpy_config.h"
#include "ch422g.h"

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lcd_drv";

static esp_lcd_panel_handle_t s_panel = NULL;
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;

/* ==================== LVGL Flush Callback ==================== */

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    int x_start = area->x1;
    int y_start = area->y1;
    int x_end = area->x2 + 1;
    int y_end = area->y2 + 1;

    esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, color_p);
    lv_disp_flush_ready(drv);
}

/* ==================== Backlight (via CH422G) ==================== */

void lcd_driver_set_backlight(uint8_t percent)
{
    /* CH422G only supports on/off, no PWM */
    ch422g_set_backlight(I2C_BUS_NUM, percent > 0);
}

/* ==================== I2C Bus Init ==================== */

static void i2c_bus_init(void)
{
    ESP_LOGI(TAG, "Initializing I2C bus (SDA=%d, SCL=%d)", I2C_BUS_SDA, I2C_BUS_SCL);

    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_BUS_SDA,
        .scl_io_num = I2C_BUS_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_BUS_FREQ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_BUS_NUM, &i2c_cfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_BUS_NUM, I2C_MODE_MASTER, 0, 0, 0));
}

/* ==================== Public API ==================== */

esp_lcd_panel_handle_t lcd_driver_init(void)
{
    ESP_LOGI(TAG, "Initializing RGB LCD panel (800x480, ST7262)");

    /*
     * Step 1: Create and init RGB panel FIRST (matches official demo order).
     * The panel hardware starts generating sync signals immediately.
     */
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_pulse_width = LCD_HSYNC,
            .hsync_back_porch = LCD_HBP,
            .hsync_front_porch = LCD_HFP,
            .vsync_pulse_width = LCD_VSYNC,
            .vsync_back_porch = LCD_VBP,
            .vsync_front_porch = LCD_VFP,
            .flags = {
                .pclk_active_neg = true,
            },
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = LCD_H_RES * 10,
        .sram_trans_align = 4,
        .psram_trans_align = 64,
        .hsync_gpio_num = LCD_PIN_HSYNC,
        .vsync_gpio_num = LCD_PIN_VSYNC,
        .de_gpio_num = LCD_PIN_DE,
        .pclk_gpio_num = LCD_PIN_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            LCD_PIN_DATA0,  LCD_PIN_DATA1,  LCD_PIN_DATA2,  LCD_PIN_DATA3,
            LCD_PIN_DATA4,  LCD_PIN_DATA5,  LCD_PIN_DATA6,  LCD_PIN_DATA7,
            LCD_PIN_DATA8,  LCD_PIN_DATA9,  LCD_PIN_DATA10, LCD_PIN_DATA11,
            LCD_PIN_DATA12, LCD_PIN_DATA13, LCD_PIN_DATA14, LCD_PIN_DATA15,
        },
        .flags = {
            .fb_in_psram = true,
        },
    };

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_LOGI(TAG, "RGB panel created and initialized");

    /*
     * Step 2: Initialize shared I2C bus
     */
    i2c_bus_init();

    /*
     * Step 3: Init GPIO4 as output (for GT911 I2C address selection)
     * Matches demo: gpio_init() sets GPIO4 as output
     */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << TOUCH_PIN_INT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /*
     * Step 4: Touch reset sequence — matches official Waveshare demo EXACTLY.
     *
     * Demo function waveshare_esp32_s3_touch_reset() does:
     *   SET = 0x01  (IO output enable)
     *   OUT = 0x2C  (BL=ON, LCD_RST=HIGH, USB_SEL=HIGH, TP_RST=LOW)
     *   delay 100ms
     *   GPIO4 = LOW (selects GT911 address 0x5D)
     *   delay 100ms
     *   OUT = 0x2E  (release TP_RST HIGH, keep BL+LCD_RST+USB_SEL)
     *   delay 200ms
     */
    ESP_ERROR_CHECK(ch422g_init(I2C_BUS_NUM));

    /* OUT = 0x2C: IO2(BL) + IO3(LCD_RST) + IO5(USB_SEL) HIGH, IO1(TP_RST) LOW */
    ESP_ERROR_CHECK(ch422g_write_io(I2C_BUS_NUM,
        CH422G_PIN_LCD_BL | CH422G_PIN_LCD_RST | CH422G_PIN_USB_SEL));
    esp_rom_delay_us(100 * 1000);  /* 100ms */

    /* GPIO4 LOW for GT911 I2C address 0x5D */
    gpio_set_level(TOUCH_PIN_INT, 0);
    esp_rom_delay_us(100 * 1000);  /* 100ms */

    /* Release touch reset: add TP_RST HIGH, keep BL + LCD_RST + USB_SEL HIGH */
    ESP_ERROR_CHECK(ch422g_set_pins(I2C_BUS_NUM, CH422G_PIN_TP_RST));
    esp_rom_delay_us(200 * 1000);  /* 200ms */

    ESP_LOGI(TAG, "LCD panel initialized, backlight ON");
    return s_panel;
}

lv_disp_t *lcd_driver_lvgl_init(esp_lcd_panel_handle_t panel)
{
    ESP_LOGI(TAG, "Initializing LVGL display driver");

    /* Allocate LVGL draw buffers in PSRAM */
    size_t buf_size = LCD_H_RES * LVGL_BUF_HEIGHT * sizeof(lv_color_t);
    lv_color_t *buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    lv_color_t *buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);

    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL buffers");
        return NULL;
    }

    lv_disp_draw_buf_init(&s_draw_buf, buf1, buf2, LCD_H_RES * LVGL_BUF_HEIGHT);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = LCD_H_RES;
    s_disp_drv.ver_res = LCD_V_RES;
    s_disp_drv.flush_cb = lvgl_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.user_data = panel;
    s_disp_drv.full_refresh = false;

    lv_disp_t *disp = lv_disp_drv_register(&s_disp_drv);
    ESP_LOGI(TAG, "LVGL display driver registered");

    return disp;
}
