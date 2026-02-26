/*
 * Custom BLE GATT Navigation Service
 *
 * Provides a writable GATT characteristic for receiving turn-by-turn
 * navigation data from a companion app/tweak on a jailbroken iPhone.
 *
 * The iPhone tweak hooks navigation apps, captures maneuver data,
 * discovers this service on the already-connected BLE peripheral,
 * and writes the data to the Nav Data characteristic.
 *
 * Data format (UTF-8 pipe-delimited):
 *   "DIR|DIST|INSTRUCTION|STREET|ETA|SPEED|APP"
 *
 * DIR values:
 *   0 = none, 1 = straight, 2 = left, 3 = right,
 *   4 = slight left, 5 = slight right, 6 = u-turn,
 *   7 = arrive, 8 = roundabout
 *
 * Special values:
 *   "NAV_END" — navigation session ended
 *
 * Example:
 *   "2|500 m|Turn left|Main St|14:30|60 km/h|Apple Maps"
 */

#include "ble_nav_service.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gatt.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ble_nav_svc";

/* ==================== Custom UUIDs (128-bit, little-endian) ==================== */

/* Service: E6A30000-B5A3-F393-E0A9-E50E24DCCA9E */
static const ble_uuid128_t nav_svc_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x00, 0x00, 0xa3, 0xe6
);

/* Nav Data: E6A30001-B5A3-F393-E0A9-E50E24DCCA9E */
static const ble_uuid128_t nav_data_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0xa3, 0xe6
);

/* ==================== State ==================== */
static ble_nav_data_t  s_nav_data = {0};
static ble_nav_data_cb_t s_nav_cb = NULL;
static bool s_inited = false;

/* ==================== Parser ==================== */

/*
 * Parse pipe-delimited nav data string.
 * Format: "DIR|DIST|INSTRUCTION|STREET|ETA|SPEED|APP"
 */
static void parse_nav_data(const char *data, uint16_t len)
{
    /* Check for NAV_END */
    if (len >= 7 && strncmp(data, "NAV_END", 7) == 0) {
        ESP_LOGI(TAG, "Navigation ended");
        memset(&s_nav_data, 0, sizeof(s_nav_data));
        s_nav_data.active = false;
        if (s_nav_cb) s_nav_cb(&s_nav_data);
        return;
    }

    /* Make a mutable copy */
    char buf[512];
    uint16_t copy_len = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, data, copy_len);
    buf[copy_len] = '\0';

    /* Parse fields */
    char *fields[7] = {0};
    int field_count = 0;
    char *p = buf;
    fields[0] = p;
    field_count = 1;

    while (*p && field_count < 7) {
        if (*p == '|') {
            *p = '\0';
            fields[field_count++] = p + 1;
        }
        p++;
    }

    if (field_count < 3) {
        ESP_LOGW(TAG, "Invalid nav data: too few fields (%d)", field_count);
        return;
    }

    /* Field 0: Direction (digit 0-8) */
    int dir = atoi(fields[0]);
    if (dir < 0 || dir > 8) dir = 0;
    s_nav_data.direction = (ble_nav_direction_t)dir;

    /* Field 1: Distance */
    if (fields[1]) {
        strncpy(s_nav_data.distance, fields[1], sizeof(s_nav_data.distance) - 1);
        s_nav_data.distance[sizeof(s_nav_data.distance) - 1] = '\0';
    }

    /* Field 2: Instruction */
    if (fields[2]) {
        strncpy(s_nav_data.instruction, fields[2], sizeof(s_nav_data.instruction) - 1);
        s_nav_data.instruction[sizeof(s_nav_data.instruction) - 1] = '\0';
    }

    /* Field 3: Street */
    if (field_count > 3 && fields[3]) {
        strncpy(s_nav_data.street, fields[3], sizeof(s_nav_data.street) - 1);
        s_nav_data.street[sizeof(s_nav_data.street) - 1] = '\0';
    }

    /* Field 4: ETA */
    if (field_count > 4 && fields[4]) {
        strncpy(s_nav_data.eta, fields[4], sizeof(s_nav_data.eta) - 1);
        s_nav_data.eta[sizeof(s_nav_data.eta) - 1] = '\0';
    }

    /* Field 5: Speed */
    if (field_count > 5 && fields[5]) {
        strncpy(s_nav_data.speed, fields[5], sizeof(s_nav_data.speed) - 1);
        s_nav_data.speed[sizeof(s_nav_data.speed) - 1] = '\0';
    }

    /* Field 6: App name */
    if (field_count > 6 && fields[6]) {
        strncpy(s_nav_data.app_name, fields[6], sizeof(s_nav_data.app_name) - 1);
        s_nav_data.app_name[sizeof(s_nav_data.app_name) - 1] = '\0';
    }

    s_nav_data.active = true;

    ESP_LOGI(TAG, "Nav: dir=%d dist='%s' instr='%s' street='%s' app='%s'",
             s_nav_data.direction, s_nav_data.distance,
             s_nav_data.instruction, s_nav_data.street, s_nav_data.app_name);

    if (s_nav_cb) s_nav_cb(&s_nav_data);
}

/* ==================== GATT Access Callback ==================== */

static int nav_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        /* Receive navigation data from phone */
        uint16_t data_len = OS_MBUF_PKTLEN(ctxt->om);
        if (data_len == 0 || data_len > 510) {
            ESP_LOGW(TAG, "Invalid write length: %d", data_len);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        char data[512];
        int rc = os_mbuf_copydata(ctxt->om, 0, data_len, data);
        if (rc != 0) {
            ESP_LOGE(TAG, "mbuf copydata failed");
            return BLE_ATT_ERR_UNLIKELY;
        }
        data[data_len] = '\0';

        ESP_LOGI(TAG, "Nav write (%d bytes): '%s'", data_len, data);
        parse_nav_data(data, data_len);
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        /* Return current nav state as a simple status string */
        const char *status = s_nav_data.active ? "ACTIVE" : "IDLE";
        os_mbuf_append(ctxt->om, status, strlen(status));
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* ==================== GATT Service Definition ==================== */

static const struct ble_gatt_svc_def nav_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = (const ble_uuid_t *)&nav_svc_uuid,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = (const ble_uuid_t *)&nav_data_uuid,
                .access_cb = nav_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_READ,
            },
            { 0 }  /* Terminator */
        },
    },
    { 0 }  /* Terminator */
};

/* ==================== Public API ==================== */

void ble_nav_service_init(void)
{
    if (s_inited) return;

    int rc = ble_gatts_count_cfg(nav_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT count failed: %d", rc);
        return;
    }

    rc = ble_gatts_add_svcs(nav_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT add svcs failed: %d", rc);
        return;
    }

    s_inited = true;
    ESP_LOGI(TAG, "Nav GATT service registered (E6A30000...)");
}

void ble_nav_service_set_cb(ble_nav_data_cb_t cb)
{
    s_nav_cb = cb;
}

const ble_nav_data_t *ble_nav_service_get_data(void)
{
    return &s_nav_data;
}

bool ble_nav_service_is_active(void)
{
    return s_nav_data.active;
}
