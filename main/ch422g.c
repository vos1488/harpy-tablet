/*
 * CH422G I/O Expander Driver (raw I2C)
 * Waveshare ESP32-S3-Touch-LCD-4.3
 */

#include "ch422g.h"
#include "driver/i2c.h"
#include "esp_log.h"

static const char *TAG = "ch422g";

/* Cache the current IO output state */
static uint8_t s_io_state = 0;

/* Write a single byte to a CH422G register (addressed by I2C slave addr) */
static esp_err_t ch422g_write_reg(int i2c_num, uint8_t reg_addr, uint8_t data)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (reg_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(i2c_num, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

esp_err_t ch422g_init(int i2c_num)
{
    ESP_LOGI(TAG, "Initializing CH422G I/O expander");

    /* Configure: IO output enable only (bit 0 = IO_OE).
     * Must match official Waveshare demo which writes 0x01 to SET register.
     * Do NOT set bit 2 (A_SCAN) — it interferes with IO operation. */
    esp_err_t ret = ch422g_write_reg(i2c_num, CH422G_REG_SET, CH422G_SET_IO_OE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure CH422G SET register: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Don't reset IO state here — caller will set the correct values */
    s_io_state = 0;

    ESP_LOGI(TAG, "CH422G initialized OK (SET=0x01)");
    return ESP_OK;
}

esp_err_t ch422g_write_io(int i2c_num, uint8_t value)
{
    s_io_state = value;
    return ch422g_write_reg(i2c_num, CH422G_REG_OUT, value);
}

uint8_t ch422g_get_io(void)
{
    return s_io_state;
}

esp_err_t ch422g_set_pins(int i2c_num, uint8_t mask)
{
    s_io_state |= mask;
    return ch422g_write_reg(i2c_num, CH422G_REG_OUT, s_io_state);
}

esp_err_t ch422g_clear_pins(int i2c_num, uint8_t mask)
{
    s_io_state &= ~mask;
    return ch422g_write_reg(i2c_num, CH422G_REG_OUT, s_io_state);
}

esp_err_t ch422g_set_backlight(int i2c_num, bool on)
{
    ESP_LOGI(TAG, "Backlight %s", on ? "ON" : "OFF");
    if (on) {
        return ch422g_set_pins(i2c_num, CH422G_PIN_LCD_BL);
    } else {
        return ch422g_clear_pins(i2c_num, CH422G_PIN_LCD_BL);
    }
}
