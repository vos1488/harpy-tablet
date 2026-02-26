/*
 * SNTP Time Manager
 * Syncs system time via NTP after WiFi connects
 */

#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include <stdbool.h>

/* Initialize SNTP (call after WiFi init) */
void time_manager_init(void);

/* Check if time has been synced */
bool time_manager_is_synced(void);

/* Get current hour and minute (local time) */
void time_manager_get_time(int *hour, int *min);

#endif /* TIME_MANAGER_H */
