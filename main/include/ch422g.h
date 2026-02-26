/*
 * CH422G I/O Expander Driver (raw I2C)
 * Used on Waveshare ESP32-S3-Touch-LCD-4.3 for:
 *   - LCD Backlight control
 *   - Touch Reset
 *   - LCD Reset
 *   - SD Card CS
 *
 * The CH422G uses multiple fixed I2C slave addresses as "registers":
 *   SET  (0x24) – system configuration
 *   OUT  (0x38) – IO0-IO7 output values
 *   EXIO (0x30) – EXIO0-EXIO3 output values
 *   IN   (0x26) – IO0-IO7 input values (read)
 */

#ifndef CH422G_H
#define CH422G_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* CH422G I2C register addresses (7-bit) */
#define CH422G_REG_SET   0x24   /* System parameter register */
#define CH422G_REG_OUT   0x38   /* IO0-IO7 output data */
#define CH422G_REG_IN    0x26   /* IO0-IO7 input data (read) */
#define CH422G_REG_EXIO  0x30   /* EXIO0-EXIO3 output data */

/* SET register bits */
#define CH422G_SET_IO_OE   (1 << 0)  /* IO0-IO7 output enable (push-pull) */
#define CH422G_SET_OD_EN   (1 << 2)  /* OC push-pull enable */

/* Waveshare ESP32-S3-Touch-LCD-4.3 CH422G pin mapping */
#define CH422G_PIN_TP_RST   (1 << 1)  /* IO1: Touch Reset */
#define CH422G_PIN_LCD_BL   (1 << 2)  /* IO2: LCD Backlight */
#define CH422G_PIN_LCD_RST  (1 << 3)  /* IO3: LCD Reset */
#define CH422G_PIN_SD_CS    (1 << 4)  /* IO4: SD Card CS */
#define CH422G_PIN_USB_SEL  (1 << 5)  /* IO5: USB Select */

/**
 * Initialize CH422G on the given I2C port.
 * The I2C bus must already be installed.
 */
esp_err_t ch422g_init(int i2c_num);

/**
 * Write IO0-IO7 output values.
 * Each bit corresponds to one IO pin.
 */
esp_err_t ch422g_write_io(int i2c_num, uint8_t value);

/**
 * Read current IO output state (cached).
 */
uint8_t ch422g_get_io(void);

/**
 * Set specific IO pins HIGH (OR with current state).
 */
esp_err_t ch422g_set_pins(int i2c_num, uint8_t mask);

/**
 * Clear specific IO pins LOW (AND-NOT with current state).
 */
esp_err_t ch422g_clear_pins(int i2c_num, uint8_t mask);

/**
 * Set backlight on/off via CH422G.
 */
esp_err_t ch422g_set_backlight(int i2c_num, bool on);

#endif /* CH422G_H */
