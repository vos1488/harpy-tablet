/*
 * SNTP Time Manager
 * Syncs system time via NTP after WiFi connects.
 * Uses pool.ntp.org with UTC+3 (Moscow) timezone.
 */

#include "time_manager.h"

#include "esp_sntp.h"
#include "esp_log.h"

#include <time.h>
#include <sys/time.h>
#include <string.h>

static const char *TAG = "time_mgr";

static bool s_synced = false;

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronized via SNTP");
    s_synced = true;
}

void time_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");

    /* Set timezone to UTC+3 (Moscow). Change as needed. */
    setenv("TZ", "MSK-3", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP initialized, waiting for sync...");
}

bool time_manager_is_synced(void)
{
    return s_synced;
}

void time_manager_get_time(int *hour, int *min)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    *hour = timeinfo.tm_hour;
    *min = timeinfo.tm_min;
}
