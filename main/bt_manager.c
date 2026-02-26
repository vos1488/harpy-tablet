/*
 * Bluetooth Manager - BLE scanning via NimBLE
 */

#include "bt_manager.h"
#include "ble_hid.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"

#include <string.h>

static const char *TAG = "bt_mgr";

static bool s_initialized = false;
static bt_scan_cb_t s_scan_cb = NULL;

#define BT_MAX_SCAN_RESULTS 30
static bt_device_info_t s_scan_results[BT_MAX_SCAN_RESULTS];
static uint16_t s_scan_count = 0;

/* BLE client connection state */
static uint16_t s_client_conn = BLE_HS_CONN_HANDLE_NONE;
static char s_conn_name[BT_DEVICE_NAME_MAX] = "";
static bt_conn_cb_t s_conn_cb = NULL;

/* ==================== BLE GAP Event Handler ==================== */

static int ble_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            /* Got advertisement */
            if (s_scan_count >= BT_MAX_SCAN_RESULTS) break;

            struct ble_hs_adv_fields fields;
            ble_hs_adv_parse_fields(&fields, event->disc.data,
                                     event->disc.length_data);

            /* Check if device already in list */
            for (int i = 0; i < s_scan_count; i++) {
                if (memcmp(s_scan_results[i].addr, event->disc.addr.val, 6) == 0) {
                    return 0; /* Already found */
                }
            }

            bt_device_info_t *dev = &s_scan_results[s_scan_count];
            memcpy(dev->addr, event->disc.addr.val, 6);
            dev->addr_type = event->disc.addr.type;
            dev->rssi = event->disc.rssi;

            if (fields.name != NULL && fields.name_len > 0) {
                int len = fields.name_len < BT_DEVICE_NAME_MAX - 1 
                    ? fields.name_len : BT_DEVICE_NAME_MAX - 1;
                memcpy(dev->name, fields.name, len);
                dev->name[len] = '\0';
            } else {
                snprintf(dev->name, BT_DEVICE_NAME_MAX, 
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         dev->addr[5], dev->addr[4], dev->addr[3],
                         dev->addr[2], dev->addr[1], dev->addr[0]);
            }

            s_scan_count++;
            ESP_LOGI(TAG, "Found: %s (RSSI: %d)", dev->name, dev->rssi);
            break;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE:
            ESP_LOGI(TAG, "Scan complete, %d devices found", s_scan_count);
            if (s_scan_cb) {
                s_scan_cb(s_scan_results, s_scan_count);
            }
            break;
        default:
            break;
    }
    return 0;
}

/* ==================== BLE Client (Central) GAP Events ==================== */

static int client_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_client_conn = event->connect.conn_handle;
                ESP_LOGI(TAG, "Connected to device: %s", s_conn_name);
                if (s_conn_cb) s_conn_cb(true, s_conn_name);
            } else {
                ESP_LOGW(TAG, "Connection failed: %d", event->connect.status);
                s_client_conn = BLE_HS_CONN_HANDLE_NONE;
                s_conn_name[0] = '\0';
                if (s_conn_cb) s_conn_cb(false, "");
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Device disconnected, reason=%d", event->disconnect.reason);
            s_client_conn = BLE_HS_CONN_HANDLE_NONE;
            s_conn_name[0] = '\0';
            if (s_conn_cb) s_conn_cb(false, "");
            break;
        default:
            break;
    }
    return 0;
}

/* ==================== NimBLE Host Task ==================== */

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_on_sync(void)
{
    /* Ensure device has a valid BLE address */
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr failed: %d", rc);
        return;
    }

    uint8_t addr_type;
    rc = ble_hs_id_infer_auto(0, &addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "BLE host synced, addr type: %d", addr_type);

    /* Tell HID module which address type to use for advertising */
    ble_hid_set_own_addr_type(addr_type);

    /* Auto-start HID advertising after host sync */
    ble_hid_start_advertising();
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason: %d", reason);
}

/* ==================== Public API ==================== */

void bt_manager_init(void)
{
    if (s_initialized) return;

    ESP_LOGI(TAG, "Initializing BLE (NimBLE)");

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return;
    }

    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    /* Register BLE HID GATT services BEFORE starting host task */
    ble_hid_init();

    nimble_port_freertos_init(ble_host_task);
    s_initialized = true;
    ESP_LOGI(TAG, "BLE initialized");
}

void bt_manager_scan(bt_scan_cb_t callback)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "BLE not initialized");
        return;
    }

    s_scan_cb = callback;
    s_scan_count = 0;
    memset(s_scan_results, 0, sizeof(s_scan_results));

    struct ble_gap_disc_params scan_params = {
        .itvl = BLE_GAP_SCAN_ITVL_MS(100),
        .window = BLE_GAP_SCAN_WIN_MS(100),
        .filter_policy = BLE_HCI_CONN_FILT_NO_WL,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 1,
    };

    ESP_LOGI(TAG, "Starting BLE scan...");
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 5000, &scan_params,
                           ble_gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE scan start failed: %d", rc);
    }
}

void bt_manager_stop_scan(void)
{
    ble_gap_disc_cancel();
}

bool bt_manager_is_enabled(void)
{
    return s_initialized;
}

void bt_manager_connect_device(const uint8_t *addr, uint8_t addr_type)
{
    if (!s_initialized) return;

    /* Stop scanning first */
    ble_gap_disc_cancel();

    /* Look up device name from scan results */
    s_conn_name[0] = '\0';
    for (int i = 0; i < s_scan_count; i++) {
        if (memcmp(s_scan_results[i].addr, addr, 6) == 0) {
            strncpy(s_conn_name, s_scan_results[i].name, BT_DEVICE_NAME_MAX - 1);
            break;
        }
    }

    ble_addr_t peer;
    peer.type = addr_type;
    memcpy(peer.val, addr, 6);

    ESP_LOGI(TAG, "Connecting to %s ...", s_conn_name);
    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer, 10000, NULL,
                             client_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect failed: %d", rc);
        if (s_conn_cb) s_conn_cb(false, "");
    }
}

void bt_manager_disconnect_device(void)
{
    if (s_client_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_client_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

bool bt_manager_is_device_connected(void)
{
    return s_client_conn != BLE_HS_CONN_HANDLE_NONE;
}

const char *bt_manager_connected_device_name(void)
{
    return s_conn_name;
}

void bt_manager_set_conn_cb(bt_conn_cb_t cb)
{
    s_conn_cb = cb;
}
