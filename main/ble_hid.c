/*
 * BLE HID Media Remote — Consumer Control (HOGP)
 * Car-stereo style pairing with passkey display on LCD.
 * Apple Media Service (AMS) client for Now Playing info.
 *
 * Crash fixes vs previous version:
 *   - send_key_task stack increased to 4096
 *   - Encryption check before sending notifications
 *   - s_send_busy flag prevents concurrent sends
 *   - Subscribe tracking (s_subscribed)
 *
 * AMS integration:
 *   - On ENC_CHANGE success → start AMS discovery
 *   - NOTIFY_RX forwarded to ble_ams module
 *
 * Bonded devices:
 *   - ble_hid_get_bonded_count() / ble_hid_get_bonded_list()
 *   - Uses NimBLE store API to enumerate NVS bonds
 */

#include "ble_hid.h"
#include "ble_ams.h"
#include "ble_ancs.h"
#include "ble_nav_service.h"
#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_uuid.h"
#include "host/ble_store.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/gap/ble_svc_gap.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* NVS bond storage init */
void ble_store_config_init(void);

static const char *TAG = "ble_hid";

/* ==================== HID Report Descriptor ==================== */
static const uint8_t hid_report_map[] = {
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (1023)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,  //   Usage Maximum (1023)
    0x75, 0x10,        //   Report Size (16 bits)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x00,        //   Input (Data, Array, Absolute)
    0xC0               // End Collection
};

/* ==================== State ==================== */
static ble_hid_state_t      s_state        = BLE_HID_DISCONNECTED;
static ble_hid_state_cb_t   s_state_cb     = NULL;
static ble_hid_passkey_cb_t s_passkey_cb   = NULL;
static uint16_t             s_conn_handle  = BLE_HS_CONN_HANDLE_NONE;
static uint16_t             s_report_handle = 0;
static char                 s_peer_name[32] = "";
static bool                 s_hid_inited   = false;
static bool                 s_encrypted    = false;  /* link encrypted? */
static bool                 s_subscribed   = false;  /* peer subscribed to notifications? */
static volatile bool        s_send_busy    = false;  /* prevent concurrent send tasks */

/* ==================== Static Data ==================== */
static const uint8_t report_ref_input[] = { 0x01, 0x01 };
static const uint8_t hid_info[] = { 0x11, 0x01, 0x00, 0x03 };
static uint8_t s_protocol_mode = 1;
/* PnP ID: src=USB-IF(2), vendor=Espressif(0x303A LE), product=1, version=1.0 */
static const uint8_t pnp_id[] = { 0x02, 0x3A, 0x30, 0x01, 0x00, 0x00, 0x01 };

/* ==================== GATT Access ==================== */
static int hid_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);

/*
 * GATT service table — Car-stereo style:
 * HID characteristics require READ_ENC + READ_AUTHEN for authenticated pairing.
 */
static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812), /* HID Service */
        .characteristics = (struct ble_gatt_chr_def[]) {
            {   .uuid = BLE_UUID16_DECLARE(0x2A4B), /* Report Map */
                .access_cb = hid_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC
                       | BLE_GATT_CHR_F_READ_AUTHEN,
            },
            {   .uuid = BLE_UUID16_DECLARE(0x2A4A), /* HID Information */
                .access_cb = hid_chr_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {   .uuid = BLE_UUID16_DECLARE(0x2A4C), /* HID Control Point */
                .access_cb = hid_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC
                       | BLE_GATT_CHR_F_WRITE_AUTHEN,
            },
            {   .uuid = BLE_UUID16_DECLARE(0x2A4E), /* Protocol Mode */
                .access_cb = hid_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {   .uuid = BLE_UUID16_DECLARE(0x2A4D), /* HID Report (Input) */
                .access_cb = hid_chr_access,
                .val_handle = &s_report_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC
                       | BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_NOTIFY,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {   .uuid = BLE_UUID16_DECLARE(0x2908),
                        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC
                                   | BLE_ATT_F_READ_AUTHEN,
                        .access_cb = hid_chr_access,
                    },
                    { 0 }
                },
            },
            { 0 }
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F), /* Battery Service */
        .characteristics = (struct ble_gatt_chr_def[]) {
            {   .uuid = BLE_UUID16_DECLARE(0x2A19),
                .access_cb = hid_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A), /* Device Information */
        .characteristics = (struct ble_gatt_chr_def[]) {
            {   .uuid = BLE_UUID16_DECLARE(0x2A29),
                .access_cb = hid_chr_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {   .uuid = BLE_UUID16_DECLARE(0x2A50),
                .access_cb = hid_chr_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            { 0 }
        },
    },
    { 0 }
};

/* ==================== GATT Access Callback ==================== */
static int hid_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint16_t uuid16 = ble_uuid_u16(ctxt->chr ? ctxt->chr->uuid : ctxt->dsc->uuid);
    switch (uuid16) {
    case 0x2A4B: os_mbuf_append(ctxt->om, hid_report_map, sizeof(hid_report_map)); break;
    case 0x2A4A: os_mbuf_append(ctxt->om, hid_info, sizeof(hid_info)); break;
    case 0x2A4C: break;
    case 0x2A4E:
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
            os_mbuf_append(ctxt->om, &s_protocol_mode, 1);
        else {
            uint8_t val;
            if (os_mbuf_copydata(ctxt->om, 0, 1, &val) == 0)
                s_protocol_mode = val;
        }
        break;
    case 0x2A4D:
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            uint8_t zero[2] = {0, 0};
            os_mbuf_append(ctxt->om, zero, sizeof(zero));
        }
        break;
    case 0x2908: os_mbuf_append(ctxt->om, report_ref_input, sizeof(report_ref_input)); break;
    case 0x2A19: { uint8_t b = 100; os_mbuf_append(ctxt->om, &b, 1); } break;
    case 0x2A29: os_mbuf_append(ctxt->om, "HARPY", 5); break;
    case 0x2A50: os_mbuf_append(ctxt->om, pnp_id, sizeof(pnp_id)); break;
    default: break;
    }
    return 0;
}

/* ==================== State helper ==================== */
static void set_state(ble_hid_state_t new_state)
{
    if (s_state != new_state) {
        s_state = new_state;
        ESP_LOGI(TAG, "State -> %d", new_state);
        if (s_state_cb) s_state_cb(new_state);
    }
}

/* ==================== Security Request Task ==================== */
static void security_request_task(void *arg)
{
    uint16_t conn_handle = (uint16_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(500));

    if (s_conn_handle == conn_handle && s_state == BLE_HID_CONNECTED) {
        ESP_LOGI(TAG, "Sending Security Request (car-stereo style)...");
        int rc = ble_gap_security_initiate(conn_handle);
        if (rc != 0) {
            ESP_LOGW(TAG, "security_initiate failed: %d", rc);
        }
    }
    vTaskDelete(NULL);
}

/* ==================== iOS Services Discovery Task ==================== */
static void ios_services_discovery_task(void *arg)
{
    uint16_t conn_handle = (uint16_t)(uintptr_t)arg;

    /* Wait for iOS to finish its own GATT discovery first */
    vTaskDelay(pdMS_TO_TICKS(1500));
    if (s_conn_handle == conn_handle && s_encrypted) {
        ESP_LOGI(TAG, "Starting AMS discovery...");
        ble_ams_start_discovery(conn_handle);
    }

    /* Wait for AMS discovery to complete, then start ANCS */
    vTaskDelay(pdMS_TO_TICKS(3000));
    if (s_conn_handle == conn_handle && s_encrypted) {
        ESP_LOGI(TAG, "Starting ANCS discovery...");
        ble_ancs_start_discovery(conn_handle);
    }

    vTaskDelete(NULL);
}

/* ==================== GAP Event Handler ==================== */
static int hid_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connection %s; status=%d",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status);
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_encrypted = false;
            s_subscribed = false;
            set_state(BLE_HID_CONNECTED);

            rc = ble_gap_conn_find(s_conn_handle, &desc);
            if (rc == 0) {
                snprintf(s_peer_name, sizeof(s_peer_name),
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         desc.peer_ota_addr.val[5], desc.peer_ota_addr.val[4],
                         desc.peer_ota_addr.val[3], desc.peer_ota_addr.val[2],
                         desc.peer_ota_addr.val[1], desc.peer_ota_addr.val[0]);

                /* If already bonded, encryption may happen automatically */
                if (desc.sec_state.encrypted) {
                    s_encrypted = true;
                    ESP_LOGI(TAG, "Already encrypted (re-connection with bonded device)");
                }
            }
            ESP_LOGI(TAG, "Connected: %s", s_peer_name);

            /* Send Security Request (car-stereo style) if not already encrypted */
            if (!s_encrypted) {
                xTaskCreate(security_request_task, "ble_sec", 2048,
                            (void *)(uintptr_t)s_conn_handle, 5, NULL);
            }
        } else {
            set_state(BLE_HID_DISCONNECTED);
            ble_hid_start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect; reason=0x%02x", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_encrypted = false;
        s_subscribed = false;
        s_send_busy = false;
        s_peer_name[0] = '\0';
        ble_ams_on_disconnected();
        ble_ancs_on_disconnected();
        set_state(BLE_HID_DISCONNECTED);
        ble_hid_start_advertising();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "conn update; status=%d", event->conn_update.status);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "adv complete; reason=%d", event->adv_complete.reason);
        if (s_state != BLE_HID_CONNECTED) {
            set_state(BLE_HID_DISCONNECTED);
            ble_hid_start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe: handle=%d notify=%d indicate=%d",
                 event->subscribe.attr_handle,
                 event->subscribe.cur_notify,
                 event->subscribe.cur_indicate);
        /* Track if peer subscribed to HID Report notifications */
        if (event->subscribe.attr_handle == s_report_handle) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "HID Report notifications %s",
                     s_subscribed ? "ENABLED" : "disabled");
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu: conn=%d val=%d",
                 event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "enc change; status=%d", event->enc_change.status);
        rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        if (rc == 0) {
            ESP_LOGI(TAG, "  encrypted=%d authenticated=%d bonded=%d key_size=%d",
                     desc.sec_state.encrypted,
                     desc.sec_state.authenticated,
                     desc.sec_state.bonded,
                     desc.sec_state.key_size);
        }
        if (event->enc_change.status == 0) {
            s_encrypted = true;
            ESP_LOGI(TAG, "*** PAIRING SUCCESSFUL — link encrypted ***");

            /* Start iOS services discovery (AMS + ANCS) after encryption */
            xTaskCreate(ios_services_discovery_task, "ios_disc", 4096,
                        (void *)(uintptr_t)event->enc_change.conn_handle, 4, NULL);
        } else {
            ESP_LOGW(TAG, "Encryption failed (status=%d) — NOT disconnecting",
                     event->enc_change.status);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        ESP_LOGI(TAG, "repeat pairing — deleting old bond and retrying");
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_NOTIFY_TX:
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        /* Forward AMS notifications to the AMS module */
        if (event->notify_rx.om) {
            uint16_t data_len = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (data_len > 0 && data_len < 512) {
                uint8_t buf[512];
                os_mbuf_copydata(event->notify_rx.om, 0, data_len, buf);
                ble_ams_handle_notify(event->notify_rx.attr_handle, buf, data_len);
                ble_ancs_handle_notify(event->notify_rx.attr_handle, buf, data_len);
            }
        }
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGI(TAG, "PASSKEY_ACTION action=%d", event->passkey.params.action);
        {
            struct ble_sm_io pkey = {0};
            pkey.action = event->passkey.params.action;

            switch (event->passkey.params.action) {
            case BLE_SM_IOACT_DISP:
                pkey.passkey = 123456;
                ESP_LOGI(TAG, "*** DISPLAY PASSKEY: %06" PRIu32 " ***", pkey.passkey);
                if (s_passkey_cb) s_passkey_cb(pkey.passkey);
                break;
            case BLE_SM_IOACT_NUMCMP:
                ESP_LOGI(TAG, "*** NUMERIC COMPARISON: %06" PRIu32 " ***",
                         event->passkey.params.numcmp);
                if (s_passkey_cb) s_passkey_cb(event->passkey.params.numcmp);
                pkey.numcmp_accept = 1;
                break;
            case BLE_SM_IOACT_INPUT:
                pkey.passkey = 123456;
                if (s_passkey_cb) s_passkey_cb(pkey.passkey);
                break;
            case BLE_SM_IOACT_OOB:
                memset(pkey.oob, 0, sizeof(pkey.oob));
                break;
            default:
                break;
            }
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "sm_inject_io result: %d", rc);
        }
        return 0;

    default:
        return 0;
    }
}

/* ==================== Send HID Report (crash-safe) ==================== */
static void send_key_task(void *arg)
{
    uint16_t usage_id = (uint16_t)(uintptr_t)arg;

    /* Safety checks */
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "Not connected, key 0x%04X dropped", usage_id);
        s_send_busy = false;
        vTaskDelete(NULL);
        return;
    }
    if (!s_encrypted) {
        ESP_LOGW(TAG, "Link not encrypted, key 0x%04X dropped", usage_id);
        s_send_busy = false;
        vTaskDelete(NULL);
        return;
    }
    if (s_report_handle == 0) {
        ESP_LOGW(TAG, "Report handle not set, key 0x%04X dropped", usage_id);
        s_send_busy = false;
        vTaskDelete(NULL);
        return;
    }

    /* Key press */
    uint8_t report[2] = { usage_id & 0xFF, (usage_id >> 8) & 0xFF };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, sizeof(report));
    if (om) {
        int rc = ble_gatts_notify_custom(s_conn_handle, s_report_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "Notify press rc=%d for key 0x%04X", rc, usage_id);
        }
    } else {
        ESP_LOGW(TAG, "mbuf alloc failed for key press");
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    /* Key release */
    uint8_t release[2] = {0, 0};
    om = ble_hs_mbuf_from_flat(release, sizeof(release));
    if (om) {
        int rc = ble_gatts_notify_custom(s_conn_handle, s_report_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "Notify release rc=%d", rc);
        }
    }

    ESP_LOGI(TAG, "Key 0x%04X sent OK", usage_id);
    s_send_busy = false;
    vTaskDelete(NULL);
}

static void send_consumer_key(uint16_t usage_id)
{
    /* Prevent concurrent sends */
    if (s_send_busy) {
        ESP_LOGW(TAG, "Send busy, key 0x%04X dropped", usage_id);
        return;
    }
    s_send_busy = true;

    /* Increased stack: 4096 (was 2048, caused stack overflow crashes) */
    BaseType_t ret = xTaskCreate(send_key_task, "hid_key", 4096,
                                  (void *)(uintptr_t)usage_id, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create send task!");
        s_send_busy = false;
    }
}

/* ==================== Bonded Devices ==================== */

int ble_hid_get_bonded_count(void)
{
    int count = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &count);
    return count;
}

int ble_hid_get_bonded_list(ble_hid_bonded_dev_t *devs, int max_devs)
{
    if (!devs || max_devs <= 0) return 0;

    int total = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &total);
    if (total == 0) return 0;

    int found = 0;
    for (int i = 0; i < total && found < max_devs; i++) {
        struct ble_store_key_sec key = {0};
        struct ble_store_value_sec val = {0};
        key.idx = i;

        int rc = ble_store_read_our_sec(&key, &val);
        if (rc != 0) break;

        memcpy(devs[found].addr, val.peer_addr.val, 6);
        devs[found].addr_type = val.peer_addr.type;
        snprintf(devs[found].name, sizeof(devs[found].name),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 val.peer_addr.val[5], val.peer_addr.val[4],
                 val.peer_addr.val[3], val.peer_addr.val[2],
                 val.peer_addr.val[1], val.peer_addr.val[0]);
        found++;
    }
    return found;
}

void ble_hid_delete_all_bonds(void)
{
    int count = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &count);
    for (int i = count - 1; i >= 0; i--) {
        struct ble_store_key_sec key = {0};
        struct ble_store_value_sec val = {0};
        key.idx = i;
        if (ble_store_read_our_sec(&key, &val) == 0) {
            ble_store_util_delete_peer(&val.peer_addr);
        }
    }
    ESP_LOGI(TAG, "All bonds deleted (%d)", count);
}

/* ==================== Public API ==================== */

void ble_hid_init(void)
{
    if (s_hid_inited) return;
    ESP_LOGI(TAG, "Initializing BLE HID (HOGP) — car-stereo pairing + AMS");

    ble_svc_gap_device_name_set("HARPY Remote");
    ble_svc_gap_device_appearance_set(0x03C0);

    /* SM config — DisplayOnly + MITM = car-stereo passkey pairing */
    ble_hs_cfg.sm_io_cap         = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding        = 1;
    ble_hs_cfg.sm_mitm           = 1;
    ble_hs_cfg.sm_sc             = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "GATT count failed: %d", rc); return; }

    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "GATT add svcs failed: %d", rc); return; }

    /* Register custom Navigation GATT service */
    ble_nav_service_init();

    s_hid_inited = true;
    ESP_LOGI(TAG, "BLE HID initialized (car-stereo + AMS + Nav)");
}

void ble_hid_start_advertising(void)
{
    if (!s_hid_inited) return;
    ble_gap_adv_stop();

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (const uint8_t *)"HARPY Remote";
    fields.name_len = strlen("HARPY Remote");
    fields.name_is_complete = 1;

    ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    fields.uuids16 = &hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv fields failed: %d", rc); return; }

    /* Scan response: appearance + ANCS/AMS service solicitation (iOS requirement) */
    /* ANCS UUID LE: 7905F431-B5CE-4E99-A40F-4B1E122D00D0 */
    static const ble_uuid128_t ancs_sol = BLE_UUID128_INIT(
        0xd0, 0x00, 0x2d, 0x12, 0x1e, 0x4b, 0x0f, 0xa4,
        0x99, 0x4e, 0xce, 0xb5, 0x31, 0xf4, 0x05, 0x79
    );

    struct ble_hs_adv_fields rsp = {0};
    rsp.appearance = 0x03C0;
    rsp.appearance_is_present = 1;
    rsp.sol_uuids128 = &ancs_sol;
    rsp.sol_num_uuids128 = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan rsp with solicitation failed: %d, trying raw", rc);
        /* Fallback: raw scan response with ANCS solicitation */
        uint8_t raw_rsp[] = {
            0x03, 0x19, 0xC0, 0x03,  /* Appearance: 0x03C0 */
            0x11, 0x15,              /* 128-bit Service Solicitation */
            0xd0, 0x00, 0x2d, 0x12, 0x1e, 0x4b, 0x0f, 0xa4,
            0x99, 0x4e, 0xce, 0xb5, 0x31, 0xf4, 0x05, 0x79,
        };
        ble_gap_adv_rsp_set_data(raw_rsp, sizeof(raw_rsp));
    }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min  = BLE_GAP_ADV_ITVL_MS(20);
    adv_params.itvl_max  = BLE_GAP_ADV_ITVL_MS(40);

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, hid_gap_event, NULL);
    if (rc != 0) { ESP_LOGE(TAG, "adv start failed: %d", rc); return; }

    set_state(BLE_HID_ADVERTISING);
    ESP_LOGI(TAG, "Advertising as 'HARPY Remote'");
}

void ble_hid_stop_advertising(void)
{
    ble_gap_adv_stop();
    if (s_state == BLE_HID_ADVERTISING) set_state(BLE_HID_DISCONNECTED);
}

void ble_hid_set_own_addr_type(uint8_t addr_type)
{
    ESP_LOGI(TAG, "addr type hint=%d (using PUBLIC)", addr_type);
}

/* Media commands */
void ble_hid_play_pause(void)  { ESP_LOGI(TAG, ">> Play/Pause");  send_consumer_key(0x00CD); }
void ble_hid_next_track(void)  { ESP_LOGI(TAG, ">> Next");        send_consumer_key(0x00B5); }
void ble_hid_prev_track(void)  { ESP_LOGI(TAG, ">> Prev");        send_consumer_key(0x00B6); }
void ble_hid_volume_up(void)   { ESP_LOGI(TAG, ">> Vol+");        send_consumer_key(0x00E9); }
void ble_hid_volume_down(void) { ESP_LOGI(TAG, ">> Vol-");        send_consumer_key(0x00EA); }
void ble_hid_mute(void)        { ESP_LOGI(TAG, ">> Mute");        send_consumer_key(0x00E2); }

/* State & callbacks */
ble_hid_state_t ble_hid_get_state(void)              { return s_state; }
void ble_hid_set_state_cb(ble_hid_state_cb_t cb)     { s_state_cb = cb; }
void ble_hid_set_passkey_cb(ble_hid_passkey_cb_t cb)  { s_passkey_cb = cb; }
const char *ble_hid_get_peer_name(void)               { return s_peer_name; }
bool ble_hid_is_encrypted(void)                       { return s_encrypted; }
