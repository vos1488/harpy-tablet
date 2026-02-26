/*
 * Touch Driver for GT911 on Waveshare ESP32-S3-Touch-LCD-4.3
 *
 * I2C bus is shared with CH422G and initialized in lcd_driver.c.
 * Touch reset is handled via CH422G I/O expander in lcd_driver.c.
 */

#include "touch_driver.h"
#include "harpy_config.h"

#include "esp_lcd_touch_gt911.h"
#include "driver/i2c.h"
#include "esp_log.h"

static const char *TAG = "touch_drv";

static esp_lcd_touch_handle_t s_touch = NULL;
static lv_indev_drv_t s_indev_drv;

/* ==================== LVGL Touch Read Callback ==================== */

static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t touch = (esp_lcd_touch_handle_t)drv->user_data;

    uint16_t x[1], y[1];
    uint16_t strength[1];
    uint8_t point_num = 0;

    esp_lcd_touch_read_data(touch);
    bool pressed;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    pressed = esp_lcd_touch_get_coordinates(touch, x, y, strength, &point_num, 1);
#pragma GCC diagnostic pop

    if (pressed && point_num > 0) {
        data->point.x = x[0];
        data->point.y = y[0];
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* ==================== Public API ==================== */

lv_indev_t *touch_driver_init(lv_disp_t *disp)
{
    ESP_LOGI(TAG, "Initializing GT911 touch");

    /* I2C bus is already initialized in lcd_driver_init() */

    /* GT911 I/O configuration */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)I2C_BUS_NUM,
                                              &io_config, &io_handle));

    esp_lcd_touch_config_t touch_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = -1,              /* reset via CH422G, not direct GPIO */
        .int_gpio_num = -1,              /* matches official demo: -1 */
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(io_handle, &touch_cfg, &s_touch));
    ESP_LOGI(TAG, "GT911 initialized");

    /* Register with LVGL */
    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = lvgl_touch_read_cb;
    s_indev_drv.user_data = s_touch;
    s_indev_drv.disp = disp;

    lv_indev_t *indev = lv_indev_drv_register(&s_indev_drv);
    ESP_LOGI(TAG, "Touch input device registered");

    return indev;
}
