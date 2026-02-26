/*
 * HARPY Tablet Firmware - Main Entry Point
 * Waveshare ESP32-S3-Touch-LCD-4.3
 *
 * Boot sequence:
 *   1. Init NVS, TCP/IP, event loop
 *   2. Init LCD + Touch + LVGL
 *   3. Show HARPY boot logo
 *   4. Init WiFi + BT + Stream subsystems
 *   5. Launch home screen (tablet UI)
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "lvgl.h"

#include "harpy_config.h"
#include "lcd_driver.h"
#include "touch_driver.h"
#include "boot_logo.h"
#include "wifi_manager.h"
#include "bt_manager.h"
#include "stream_viewer.h"
#include "ui_home.h"
#include "time_manager.h"

static const char *TAG = "harpy";

static SemaphoreHandle_t s_lvgl_mutex = NULL;
static lv_disp_t *s_disp = NULL;

/* ==================== LVGL Tick ==================== */

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(2);
}

/* ==================== WiFi State Callback ==================== */

static void on_wifi_state_change(wifi_state_t state)
{
    bool connected = (state == WIFI_STATE_CONNECTED);
    const char *ip = wifi_manager_get_ip();
    ESP_LOGI(TAG, "WiFi state: %d, IP: %s", state, ip);

    /* Start SNTP on first connect */
    static bool sntp_started = false;
    if (connected && !sntp_started) {
        time_manager_init();
        sntp_started = true;
    }

    if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        ui_home_update_wifi_status(connected, ip);
        xSemaphoreGive(s_lvgl_mutex);
    }
}

/* ==================== Clock Update Timer (LVGL) ==================== */

static void clock_update_timer_cb(lv_timer_t *timer)
{
    if (time_manager_is_synced()) {
        int h, m;
        time_manager_get_time(&h, &m);
        ui_home_update_time(h, m);
    }
}

/* ==================== Boot Complete → Launch Home ==================== */

static void on_boot_complete(void)
{
    ESP_LOGI(TAG, "Boot complete, launching home screen");

    /* Create home screen on the active display */
    lv_obj_t *scr = lv_disp_get_scr_act(s_disp);
    ui_home_create(scr);

    /* Update BT status */
    ui_home_update_bt_status(bt_manager_is_enabled());

    /* Start clock update timer (every 1 second) */
    lv_timer_create(clock_update_timer_cb, 1000, NULL);

    /* Try auto-connect WiFi */
    wifi_manager_auto_connect();
}

/* ==================== LVGL Task ==================== */

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started");

    while (1) {
        if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            uint32_t time_till_next = lv_timer_handler();
            xSemaphoreGive(s_lvgl_mutex);
            
            if (time_till_next > 50) time_till_next = 50;
            vTaskDelay(pdMS_TO_TICKS(time_till_next));
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

/* ==================== App Main ==================== */

void app_main(void)
{
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║     HARPY Tablet System v1.0      ║");
    ESP_LOGI(TAG, "║  ESP32-S3 Touch LCD 4.3\"          ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");

    /* ===== 1. System Init ===== */
    ESP_LOGI(TAG, "Step 1: System initialization");

    /* NVS Flash */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* TCP/IP + Event Loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* ===== 2. Display Init ===== */
    ESP_LOGI(TAG, "Step 2: Display initialization");

    /* Initialize LVGL library */
    lv_init();
    s_lvgl_mutex = xSemaphoreCreateMutex();

    /* LVGL tick timer (2ms) */
    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 2000)); /* 2ms */

    /* Init LCD hardware */
    esp_lcd_panel_handle_t panel = lcd_driver_init();

    /* Init LVGL display driver */
    s_disp = lcd_driver_lvgl_init(panel);
    if (!s_disp) {
        ESP_LOGE(TAG, "Failed to init LVGL display!");
        return;
    }

    /* Init touch */
    lv_indev_t *indev = touch_driver_init(s_disp);
    if (!indev) {
        ESP_LOGW(TAG, "Touch init failed, continuing without touch");
    }

    /* ===== 3. Boot Logo ===== */
    ESP_LOGI(TAG, "Step 3: Showing boot logo");

    if (xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
        lv_obj_t *scr = lv_disp_get_scr_act(s_disp);
        boot_logo_show(scr, on_boot_complete);
        xSemaphoreGive(s_lvgl_mutex);
    }

    /* Start LVGL task */
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL,
                             LVGL_TASK_PRIORITY, NULL, 0);

    /* ===== 4. WiFi + BT + Stream Init (while boot logo plays) ===== */
    ESP_LOGI(TAG, "Step 4: WiFi + BT + Stream initialization");

    wifi_manager_init();
    wifi_manager_set_state_cb(on_wifi_state_change);

    bt_manager_init();

    stream_viewer_init();

    ESP_LOGI(TAG, "All subsystems initialized, boot logo playing...");

    /* Main task can end - everything runs in FreeRTOS tasks */
}
