#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_wifi_types.h"

#define WIFI_SSID_MAX_LEN   33
#define WIFI_PASS_MAX_LEN   65

typedef enum {
    WIFI_STATE_DISCONNECTED = 0,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_FAILED,
} wifi_state_t;

typedef struct {
    char ssid[WIFI_SSID_MAX_LEN];
    int8_t rssi;
    wifi_auth_mode_t auth;
} wifi_ap_info_t;

typedef void (*wifi_state_cb_t)(wifi_state_t state);
typedef void (*wifi_scan_cb_t)(wifi_ap_info_t *ap_list, uint16_t count);

/* Initialize WiFi subsystem */
void wifi_manager_init(void);

/* Start scanning for networks */
void wifi_manager_scan(wifi_scan_cb_t callback);

/* Connect to a network */
void wifi_manager_connect(const char *ssid, const char *password);

/* Disconnect */
void wifi_manager_disconnect(void);

/* Get current state */
wifi_state_t wifi_manager_get_state(void);

/* Get current IP address string */
const char *wifi_manager_get_ip(void);

/* Register state change callback */
void wifi_manager_set_state_cb(wifi_state_cb_t cb);

/* Save credentials to NVS */
void wifi_manager_save_credentials(const char *ssid, const char *password);

/* Load and auto-connect saved network */
void wifi_manager_auto_connect(void);

/* ==================== AP Mode ==================== */

/* Start SoftAP (hotspot) with given SSID and password */
void wifi_manager_start_ap(const char *ssid, const char *password);

/* Stop SoftAP */
void wifi_manager_stop_ap(void);

/* Check if AP is running */
bool wifi_manager_ap_is_active(void);

/* Get AP IP address string (usually "192.168.4.1") */
const char *wifi_manager_get_ap_ip(void);

/* Get number of connected stations */
int wifi_manager_get_ap_sta_count(void);

/* ==================== Network Info ==================== */

/* Get MAC address as string "AA:BB:CC:DD:EE:FF" */
void wifi_manager_get_mac(char *buf, size_t len);

/* Get RSSI of current STA connection */
int wifi_manager_get_rssi(void);

/* Get gateway IP string */
void wifi_manager_get_gateway(char *buf, size_t len);

/* Get DNS IP string */
void wifi_manager_get_dns(char *buf, size_t len);

/* Get connected SSID */
void wifi_manager_get_ssid(char *buf, size_t len);

#endif /* WIFI_MANAGER_H */
