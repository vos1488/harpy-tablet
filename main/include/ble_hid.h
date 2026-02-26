/*
 * BLE HID Media Remote — Consumer Control (HOGP)
 *
 * Car-stereo style pairing with passkey display.
 * Apple Media Service (AMS) client for Now Playing info.
 * Bonded device list stored in NVS.
 */

#ifndef BLE_HID_H
#define BLE_HID_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BLE_HID_DISCONNECTED = 0,
    BLE_HID_ADVERTISING,
    BLE_HID_CONNECTED,
} ble_hid_state_t;

typedef void (*ble_hid_state_cb_t)(ble_hid_state_t state);
typedef void (*ble_hid_passkey_cb_t)(uint32_t passkey);

/* Bonded device info */
typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    char    name[32];   /* address string or resolved name */
} ble_hid_bonded_dev_t;

/* Initialize HID service (call AFTER bt_manager_init) */
void ble_hid_init(void);

/* Start advertising so phone can find us */
void ble_hid_start_advertising(void);

/* Stop advertising */
void ble_hid_stop_advertising(void);

/* Set own BLE address type (call before start_advertising) */
void ble_hid_set_own_addr_type(uint8_t addr_type);

/* Media control commands */
void ble_hid_play_pause(void);
void ble_hid_next_track(void);
void ble_hid_prev_track(void);
void ble_hid_volume_up(void);
void ble_hid_volume_down(void);
void ble_hid_mute(void);

/* State */
ble_hid_state_t ble_hid_get_state(void);
void ble_hid_set_state_cb(ble_hid_state_cb_t cb);

/* Set callback for passkey display (car-stereo pairing) */
void ble_hid_set_passkey_cb(ble_hid_passkey_cb_t cb);

/* Get connected device name/address */
const char *ble_hid_get_peer_name(void);

/* Is link encrypted and ready for HID reports? */
bool ble_hid_is_encrypted(void);

/* Bonded devices list (stored in NVS) */
int ble_hid_get_bonded_count(void);
int ble_hid_get_bonded_list(ble_hid_bonded_dev_t *devs, int max_devs);
void ble_hid_delete_all_bonds(void);

#endif /* BLE_HID_H */
