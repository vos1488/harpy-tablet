#ifndef BT_MANAGER_H
#define BT_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#define BT_DEVICE_NAME_MAX  64

typedef struct {
    char name[BT_DEVICE_NAME_MAX];
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
} bt_device_info_t;

typedef void (*bt_scan_cb_t)(bt_device_info_t *devices, uint16_t count);
typedef void (*bt_conn_cb_t)(bool connected, const char *name);

/* Initialize BLE */
void bt_manager_init(void);

/* Start BLE scan */
void bt_manager_scan(bt_scan_cb_t callback);

/* Stop BLE scan */
void bt_manager_stop_scan(void);

/* Check if BT is enabled */
bool bt_manager_is_enabled(void);

/* Connect to a BLE device (central role) */
void bt_manager_connect_device(const uint8_t *addr, uint8_t addr_type);

/* Disconnect from connected device */
void bt_manager_disconnect_device(void);

/* Check if a peripheral device is connected */
bool bt_manager_is_device_connected(void);

/* Get connected device name */
const char *bt_manager_connected_device_name(void);

/* Set connection state callback */
void bt_manager_set_conn_cb(bt_conn_cb_t cb);

#endif /* BT_MANAGER_H */
