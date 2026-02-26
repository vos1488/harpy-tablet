/*
 * Custom BLE GATT Navigation Service
 *
 * Receives turn-by-turn navigation data from a jailbroken iPhone
 * via a custom writable GATT characteristic.
 *
 * Protocol:
 *   Write UTF-8 string to Nav Data characteristic:
 *     "DIR|DIST|INSTRUCTION|STREET|ETA|SPEED|APP"
 *   Where DIR is 0-8 (direction enum).
 *   Write "NAV_END" to signal navigation ended.
 *
 * Service UUID:  E6A30000-B5A3-F393-E0A9-E50E24DCCA9E
 * Nav Data UUID: E6A30001-B5A3-F393-E0A9-E50E24DCCA9E
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Navigation direction */
typedef enum {
    BLE_NAV_DIR_NONE = 0,
    BLE_NAV_DIR_STRAIGHT,
    BLE_NAV_DIR_LEFT,
    BLE_NAV_DIR_RIGHT,
    BLE_NAV_DIR_SLIGHT_LEFT,
    BLE_NAV_DIR_SLIGHT_RIGHT,
    BLE_NAV_DIR_UTURN,
    BLE_NAV_DIR_ARRIVE,
    BLE_NAV_DIR_ROUNDABOUT,
} ble_nav_direction_t;

/* Navigation data received from phone */
typedef struct {
    ble_nav_direction_t direction;
    char distance[32];       /* e.g. "500 m", "1.2 km" */
    char instruction[128];   /* e.g. "Turn left", "Поверните налево" */
    char street[128];        /* e.g. "ул. Ленина" */
    char eta[16];            /* e.g. "14:30" */
    char speed[16];          /* e.g. "60 km/h" */
    char app_name[32];       /* e.g. "Apple Maps" */
    bool active;             /* Navigation is active */
} ble_nav_data_t;

/* Callback when navigation data updates */
typedef void (*ble_nav_data_cb_t)(const ble_nav_data_t *data);

/* Initialize the nav GATT service (call before NimBLE host sync) */
void ble_nav_service_init(void);

/* Set callback for navigation data updates */
void ble_nav_service_set_cb(ble_nav_data_cb_t cb);

/* Get current navigation data */
const ble_nav_data_t *ble_nav_service_get_data(void);

/* Whether navigation is active */
bool ble_nav_service_is_active(void);
