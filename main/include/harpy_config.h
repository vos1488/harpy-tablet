/*
 * HARPY Tablet Firmware
 * Waveshare ESP32-S3-Touch-LCD-4.3
 * 
 * Hardware: ESP32-S3-N16R8, 4.3" 800x480 RGB LCD (ST7262), GT911 Touch
 *           CH422G I/O Expander for backlight/reset control
 */

#ifndef HARPY_CONFIG_H
#define HARPY_CONFIG_H

/* ==================== Display Configuration ==================== */
#define LCD_H_RES           800
#define LCD_V_RES           480
#define LCD_BIT_PER_PIXEL   16

/* RGB LCD Timing - Waveshare 4.3" ST7262 (official values) */
#define LCD_HSYNC           4
#define LCD_HBP             8
#define LCD_HFP             8
#define LCD_VSYNC           4
#define LCD_VBP             8
#define LCD_VFP             8

#define LCD_PCLK_HZ         (16 * 1000 * 1000)  /* 16 MHz pixel clock */

/* RGB LCD Pin Configuration - Waveshare ESP32-S3-Touch-LCD-4.3 (CONFIRMED) */
#define LCD_PIN_HSYNC       46
#define LCD_PIN_VSYNC       3
#define LCD_PIN_DE          5
#define LCD_PIN_PCLK        7

/* 16-bit RGB565 data bus: DATA0..DATA15 */
#define LCD_PIN_DATA0       14  /* B3 */
#define LCD_PIN_DATA1       38  /* B4 */
#define LCD_PIN_DATA2       18  /* B5 */
#define LCD_PIN_DATA3       17  /* B6 */
#define LCD_PIN_DATA4       10  /* B7 */
#define LCD_PIN_DATA5       39  /* G2 */
#define LCD_PIN_DATA6       0   /* G3 */
#define LCD_PIN_DATA7       45  /* G4 */
#define LCD_PIN_DATA8       48  /* G5 */
#define LCD_PIN_DATA9       47  /* G6 */
#define LCD_PIN_DATA10      21  /* G7 */
#define LCD_PIN_DATA11      1   /* R3 */
#define LCD_PIN_DATA12      2   /* R4 */
#define LCD_PIN_DATA13      42  /* R5 */
#define LCD_PIN_DATA14      41  /* R6 */
#define LCD_PIN_DATA15      40  /* R7 */

/* Backlight via CH422G I/O Expander (NOT direct GPIO!) */
/* See ch422g.h for backlight control API */

/* ==================== I2C Bus Configuration ==================== */
#define I2C_BUS_NUM         0
#define I2C_BUS_SDA         8
#define I2C_BUS_SCL         9
#define I2C_BUS_FREQ        (400 * 1000)

/* ==================== Touch Configuration ==================== */
#define TOUCH_PIN_INT       4     /* GT911 INT pin */
#define TOUCH_PIN_RST       (-1)  /* RST via CH422G IO expander, not direct GPIO */
#define TOUCH_GT911_ADDR    0x5D

/* ==================== LVGL Configuration ==================== */
#define LVGL_BUF_HEIGHT     100   /* Lines for double buffer */
#define LVGL_TASK_STACK     (8 * 1024)
#define LVGL_TASK_PRIORITY  5

/* ==================== WiFi Configuration ==================== */
#define WIFI_MAX_RETRY      5
#define WIFI_SCAN_MAX_AP    20
#define WIFI_NVS_NAMESPACE  "wifi_cfg"

/* ==================== Stream Configuration ==================== */
#define STREAM_BUF_SIZE     (120 * 1024)
#define STREAM_TASK_STACK   (16 * 1024)
#define STREAM_TASK_PRIORITY 4

/* ==================== SD Card (SPI) Configuration ==================== */
#define SD_PIN_MOSI         11
#define SD_PIN_MISO         13
#define SD_PIN_CLK          12
/* SD CS is on CH422G IO4 — not a direct GPIO */
#define SD_SPI_HOST         SPI2_HOST
#define SD_MOUNT_POINT      "/sdcard"
#define SD_MAX_FILES        5

/* ==================== RS485 (UART2) Configuration ==================== */
#define RS485_TX_PIN        16
#define RS485_RX_PIN        15
#define RS485_UART_NUM      UART_NUM_2
#define RS485_DEFAULT_BAUD  115200
#define RS485_BUF_SIZE      1024

/* ==================== ADC Sensor Configuration ==================== */
#define ADC_SENSOR_GPIO     6           /* GPIO 6 = ADC1 CH5 */
#define ADC_SENSOR_CHANNEL  ADC_CHANNEL_5   /* GPIO 6 */
#define ADC_SENSOR_UNIT     ADC_UNIT_1
#define ADC_SENSOR_ATTEN    ADC_ATTEN_DB_12

/* ==================== CAN/TWAI Configuration ==================== */
#define CAN_TX_PIN          20
#define CAN_RX_PIN          19
/* USB_SEL on CH422G IO5 must be HIGH for CAN mode */

/* ==================== Colors ==================== */
#define HARPY_COLOR_BG      lv_color_hex(0x0D1117)
#define HARPY_COLOR_CARD    lv_color_hex(0x161B22)
#define HARPY_COLOR_PRIMARY lv_color_hex(0x58A6FF)
#define HARPY_COLOR_ACCENT  lv_color_hex(0x7C3AED)
#define HARPY_COLOR_SUCCESS lv_color_hex(0x3FB950)
#define HARPY_COLOR_WARN    lv_color_hex(0xD29922)
#define HARPY_COLOR_ERROR   lv_color_hex(0xF85149)
#define HARPY_COLOR_TEXT    lv_color_hex(0xC9D1D9)
#define HARPY_COLOR_MUTED   lv_color_hex(0x8B949E)

#endif /* HARPY_CONFIG_H */
