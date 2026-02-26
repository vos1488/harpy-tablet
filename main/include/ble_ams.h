/*
 * Apple Media Service (AMS) Client
 *
 * Connects to the AMS GATT service on an iPhone to receive
 * Now Playing media information (title, artist, album, playback state).
 *
 * AMS is available after bonding with an iOS device that has music playing.
 */

#ifndef BLE_AMS_H
#define BLE_AMS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char title[128];
    char artist[128];
    char album[128];
    bool playing;
} ble_ams_media_info_t;

typedef void (*ble_ams_media_cb_t)(const ble_ams_media_info_t *info);

/* Start AMS discovery on the connected peer (call after encryption success) */
void ble_ams_start_discovery(uint16_t conn_handle);

/* Notify AMS module of disconnection */
void ble_ams_on_disconnected(void);

/* Forward NOTIFY_RX events from the GAP handler to AMS */
void ble_ams_handle_notify(uint16_t attr_handle, const uint8_t *data, uint16_t len);

/* Set callback for media info updates */
void ble_ams_set_media_cb(ble_ams_media_cb_t cb);

/* Get current media info (may be empty if not discovered) */
const ble_ams_media_info_t *ble_ams_get_media_info(void);

/* Is AMS discovered and subscribed? */
bool ble_ams_is_active(void);

#endif /* BLE_AMS_H */
