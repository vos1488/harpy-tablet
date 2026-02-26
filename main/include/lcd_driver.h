#ifndef LCD_DRIVER_H
#define LCD_DRIVER_H

#include "esp_lcd_types.h"
#include "lvgl.h"

/* Initialize the RGB LCD panel and backlight */
esp_lcd_panel_handle_t lcd_driver_init(void);

/* Initialize LVGL display driver with the panel */
lv_disp_t *lcd_driver_lvgl_init(esp_lcd_panel_handle_t panel);

/* Set backlight level (0-100) */
void lcd_driver_set_backlight(uint8_t percent);

#endif /* LCD_DRIVER_H */
