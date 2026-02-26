/*
 * Apple Notification Center Service (ANCS) Client
 *
 * GATT client that subscribes to iOS notifications.
 * Used to receive turn-by-turn navigation instructions from
 * Apple Maps, Google Maps, Waze, Yandex Navigator, 2GIS, etc.
 *
 * ANCS Service UUID: 7905F431-B5CE-4E99-A40F-4B1E122D00D0
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Navigation direction parsed from notification text */
typedef enum {
    NAV_DIR_NONE = 0,
    NAV_DIR_STRAIGHT,
    NAV_DIR_LEFT,
    NAV_DIR_RIGHT,
    NAV_DIR_SLIGHT_LEFT,
    NAV_DIR_SLIGHT_RIGHT,
    NAV_DIR_UTURN,
    NAV_DIR_ARRIVE,
    NAV_DIR_ROUNDABOUT,
} nav_direction_t;

/* Navigation info extracted from ANCS notification */
typedef struct {
    char title[128];          /* Main instruction (e.g. "Turn left") */
    char subtitle[128];       /* Additional info */
    char message[256];        /* Detail (e.g. street name, distance) */
    char app_name[32];        /* Source app short name */
    nav_direction_t direction;
    bool active;              /* Navigation notification is live */
} ble_ancs_nav_info_t;

/* Callback when navigation info updates */
typedef void (*ble_ancs_nav_cb_t)(const ble_ancs_nav_info_t *info);

/* Start ANCS discovery on a connected & encrypted peer */
void ble_ancs_start_discovery(uint16_t conn_handle);

/* Clean up on disconnect */
void ble_ancs_on_disconnected(void);

/* Forward GATT notifications from GAP event handler */
void ble_ancs_handle_notify(uint16_t attr_handle, const uint8_t *data, uint16_t len);

/* Set/get navigation callback */
void ble_ancs_set_nav_cb(ble_ancs_nav_cb_t cb);

/* Get current navigation info (may be inactive) */
const ble_ancs_nav_info_t *ble_ancs_get_nav_info(void);

/* Whether ANCS service was discovered and subscribed */
bool ble_ancs_is_active(void);
