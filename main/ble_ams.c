/*
 * Apple Media Service (AMS) Client
 *
 * GATT client that discovers AMS on the connected iPhone and subscribes
 * to Entity Update notifications to receive Now Playing info.
 *
 * Discovery chain (async callbacks):
 *   1. Discover AMS service by UUID
 *   2. Discover all characteristics within the service
 *   3. Discover descriptors (CCCD) for Entity Update
 *   4. Write CCCD to enable notifications
 *   5. Write Entity Update to subscribe for Track + Player info
 *   6. Receive notifications with media data
 *
 * AMS Service UUID:           89D3502B-0F36-433A-8EF4-C502AD55F8DC
 * Remote Command char:        9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2
 * Entity Update char (notify): 2F7CABCE-808D-411F-9A0C-BB92BA96C102
 * Entity Attribute char:       C6B2F38C-23AB-46D8-A6AB-A3A870BBD5D7
 */

#include "ble_ams.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include <string.h>

static const char *TAG = "ble_ams";

/* ==================== AMS UUIDs (128-bit, little-endian) ==================== */

/* 89D3502B-0F36-433A-8EF4-C502AD55F8DC */
static const ble_uuid128_t ams_svc_uuid = BLE_UUID128_INIT(
    0xdc, 0xf8, 0x55, 0xad, 0x02, 0xc5, 0xf4, 0x8e,
    0x3a, 0x43, 0x36, 0x0f, 0x2b, 0x50, 0xd3, 0x89
);

/* 2F7CABCE-808D-411F-9A0C-BB92BA96C102 — Entity Update */
static const ble_uuid128_t ams_entity_update_uuid = BLE_UUID128_INIT(
    0x02, 0xc1, 0x96, 0xba, 0x92, 0xbb, 0x0c, 0x9a,
    0x1f, 0x41, 0x8d, 0x80, 0xce, 0xab, 0x7c, 0x2f
);

/* 9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2 — Remote Command */
static const ble_uuid128_t ams_remote_cmd_uuid = BLE_UUID128_INIT(
    0xc2, 0x51, 0xca, 0xf7, 0x56, 0x0e, 0xdf, 0xb8,
    0x8a, 0x4a, 0xb1, 0x57, 0xd8, 0x81, 0x3c, 0x9b
);

/* ==================== State ==================== */
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_svc_start = 0;
static uint16_t s_svc_end = 0;
static uint16_t s_entity_update_handle = 0;
static uint16_t s_entity_update_cccd = 0;
static uint16_t s_remote_cmd_handle = 0;
static bool     s_active = false;

static ble_ams_media_info_t s_media = {0};
static ble_ams_media_cb_t   s_media_cb = NULL;

/* ==================== Forward declarations ==================== */
static int ams_svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *service, void *arg);
static int ams_chr_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg);
static int ams_dsc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg);
static int ams_cccd_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg);
static int ams_entity_sub_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg);

/* ==================== Discovery chain ==================== */

/* Step 1: Discover AMS service */
void ble_ams_start_discovery(uint16_t conn_handle)
{
    s_conn_handle = conn_handle;
    s_entity_update_handle = 0;
    s_entity_update_cccd = 0;
    s_remote_cmd_handle = 0;
    s_active = false;

    ESP_LOGI(TAG, "Starting AMS service discovery...");
    int rc = ble_gattc_disc_svc_by_uuid(conn_handle,
                                         (const ble_uuid_t *)&ams_svc_uuid,
                                         ams_svc_disc_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "AMS service discovery start failed: %d", rc);
    }
}

/* Step 1 callback: service found → discover characteristics */
static int ams_svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *service, void *arg)
{
    if (error->status == 0 && service) {
        s_svc_start = service->start_handle;
        s_svc_end = service->end_handle;
        ESP_LOGI(TAG, "AMS service found: handles %d-%d", s_svc_start, s_svc_end);
    } else if (error->status == BLE_HS_EDONE) {
        /* Discovery done */
        if (s_svc_start == 0) {
            ESP_LOGW(TAG, "AMS service NOT found on this device");
            return 0;
        }
        ESP_LOGI(TAG, "Discovering AMS characteristics...");
        int rc = ble_gattc_disc_all_chrs(conn_handle, s_svc_start, s_svc_end,
                                          ams_chr_disc_cb, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "AMS chr discovery failed: %d", rc);
        }
    } else {
        ESP_LOGW(TAG, "AMS svc discovery error: %d", error->status);
    }
    return 0;
}

/* Step 2 callback: characteristics found → discover descriptors */
static int ams_chr_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0 && chr) {
        if (ble_uuid_cmp(&chr->uuid.u, (const ble_uuid_t *)&ams_entity_update_uuid) == 0) {
            s_entity_update_handle = chr->val_handle;
            ESP_LOGI(TAG, "Entity Update chr: val_handle=%d", chr->val_handle);
        } else if (ble_uuid_cmp(&chr->uuid.u, (const ble_uuid_t *)&ams_remote_cmd_uuid) == 0) {
            s_remote_cmd_handle = chr->val_handle;
            ESP_LOGI(TAG, "Remote Command chr: val_handle=%d", chr->val_handle);
        }
    } else if (error->status == BLE_HS_EDONE) {
        if (s_entity_update_handle == 0) {
            ESP_LOGW(TAG, "Entity Update characteristic not found");
            return 0;
        }
        /* Discover descriptors for Entity Update to find CCCD */
        uint16_t dsc_end = s_svc_end;
        ESP_LOGI(TAG, "Discovering Entity Update descriptors...");
        int rc = ble_gattc_disc_all_dscs(conn_handle,
                                          s_entity_update_handle, dsc_end,
                                          ams_dsc_disc_cb, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "AMS dsc discovery failed: %d", rc);
        }
    } else {
        ESP_LOGW(TAG, "AMS chr discovery error: %d", error->status);
    }
    return 0;
}

/* Step 3 callback: descriptors found → write CCCD */
static int ams_dsc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    if (error->status == 0 && dsc) {
        if (ble_uuid_u16(&dsc->uuid.u) == 0x2902) {  /* CCCD */
            s_entity_update_cccd = dsc->handle;
            ESP_LOGI(TAG, "Entity Update CCCD: handle=%d", dsc->handle);
        }
    } else if (error->status == BLE_HS_EDONE) {
        if (s_entity_update_cccd == 0) {
            ESP_LOGW(TAG, "Entity Update CCCD not found");
            return 0;
        }
        /* Enable notifications on Entity Update */
        uint8_t cccd_val[2] = {0x01, 0x00}; /* notifications ON */
        ESP_LOGI(TAG, "Enabling Entity Update notifications (CCCD write)...");
        int rc = ble_gattc_write_flat(conn_handle, s_entity_update_cccd,
                                       cccd_val, sizeof(cccd_val),
                                       ams_cccd_write_cb, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "CCCD write failed: %d", rc);
        }
    } else {
        ESP_LOGW(TAG, "AMS dsc discovery error: %d", error->status);
    }
    return 0;
}

/* Step 4 callback: CCCD written → subscribe to Track + Player updates */
static int ams_cccd_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        ESP_LOGW(TAG, "CCCD write error: %d", error->status);
        return 0;
    }
    ESP_LOGI(TAG, "Entity Update notifications enabled");

    /*
     * Subscribe for Track entity updates:
     *   EntityID = 0x02 (Track)
     *   Attributes: 0x00=Artist, 0x01=Album, 0x02=Title
     */
    uint8_t track_sub[] = {0x02, 0x00, 0x01, 0x02};
    ESP_LOGI(TAG, "Subscribing to Track entity updates...");
    int rc = ble_gattc_write_flat(conn_handle, s_entity_update_handle,
                                   track_sub, sizeof(track_sub),
                                   ams_entity_sub_cb, (void *)0x02);
    if (rc != 0) {
        ESP_LOGW(TAG, "Track subscribe write failed: %d", rc);
    }
    return 0;
}

/* Step 5 callback: entity subscription written */
static int ams_entity_sub_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg)
{
    uintptr_t entity_id = (uintptr_t)arg;
    if (error->status != 0) {
        ESP_LOGW(TAG, "Entity subscribe error: %d (entity=0x%02x)",
                 error->status, (unsigned)entity_id);
        return 0;
    }

    if (entity_id == 0x02) {
        ESP_LOGI(TAG, "Track subscription active");
        /*
         * Now subscribe for Player entity updates:
         *   EntityID = 0x00 (Player)
         *   Attributes: 0x01=PlaybackInfo
         */
        uint8_t player_sub[] = {0x00, 0x01};
        int rc = ble_gattc_write_flat(conn_handle, s_entity_update_handle,
                                       player_sub, sizeof(player_sub),
                                       ams_entity_sub_cb, (void *)0x00);
        if (rc != 0) {
            ESP_LOGW(TAG, "Player subscribe write failed: %d", rc);
        }
    } else {
        ESP_LOGI(TAG, "Player subscription active — AMS fully configured!");
        s_active = true;
    }
    return 0;
}

/* ==================== Notification handling ==================== */

void ble_ams_handle_notify(uint16_t attr_handle, const uint8_t *data, uint16_t len)
{
    if (attr_handle != s_entity_update_handle || len < 3) return;

    /*
     * Entity Update notification format:
     *   byte 0: EntityID (0x00=Player, 0x01=Queue, 0x02=Track)
     *   byte 1: AttributeID
     *   byte 2: EntityUpdateFlags (0x01=truncated)
     *   bytes 3...: UTF-8 value
     */
    uint8_t entity_id = data[0];
    uint8_t attr_id = data[1];
    /* uint8_t flags = data[2]; */
    const char *value = (len > 3) ? (const char *)&data[3] : "";
    uint16_t value_len = (len > 3) ? (len - 3) : 0;

    bool changed = false;

    if (entity_id == 0x02) {
        /* Track entity */
        switch (attr_id) {
        case 0x00: /* Artist */
            memset(s_media.artist, 0, sizeof(s_media.artist));
            if (value_len > 0) {
                size_t cpy = value_len < sizeof(s_media.artist) - 1 ? value_len : sizeof(s_media.artist) - 1;
                memcpy(s_media.artist, value, cpy);
            }
            ESP_LOGI(TAG, "Track Artist: %s", s_media.artist);
            changed = true;
            break;
        case 0x01: /* Album */
            memset(s_media.album, 0, sizeof(s_media.album));
            if (value_len > 0) {
                size_t cpy = value_len < sizeof(s_media.album) - 1 ? value_len : sizeof(s_media.album) - 1;
                memcpy(s_media.album, value, cpy);
            }
            ESP_LOGI(TAG, "Track Album: %s", s_media.album);
            changed = true;
            break;
        case 0x02: /* Title */
            memset(s_media.title, 0, sizeof(s_media.title));
            if (value_len > 0) {
                size_t cpy = value_len < sizeof(s_media.title) - 1 ? value_len : sizeof(s_media.title) - 1;
                memcpy(s_media.title, value, cpy);
            }
            ESP_LOGI(TAG, "Track Title: %s", s_media.title);
            changed = true;
            break;
        default:
            break;
        }
    } else if (entity_id == 0x00) {
        /* Player entity */
        if (attr_id == 0x01) {
            /* PlaybackInfo: "PlaybackState,PlaybackRate,ElapsedTime" */
            if (value_len > 0) {
                int pb_state = value[0] - '0';
                s_media.playing = (pb_state == 1);
                ESP_LOGI(TAG, "PlaybackState: %d (%s)",
                         pb_state, s_media.playing ? "Playing" : "Paused");
                changed = true;
            }
        }
    }

    if (changed && s_media_cb) {
        s_media_cb(&s_media);
    }
}

/* ==================== Public API ==================== */

void ble_ams_on_disconnected(void)
{
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_svc_start = 0;
    s_svc_end = 0;
    s_entity_update_handle = 0;
    s_entity_update_cccd = 0;
    s_remote_cmd_handle = 0;
    s_active = false;
    memset(&s_media, 0, sizeof(s_media));
    ESP_LOGI(TAG, "AMS reset (disconnected)");
    if (s_media_cb) {
        s_media_cb(&s_media);
    }
}

void ble_ams_set_media_cb(ble_ams_media_cb_t cb)
{
    s_media_cb = cb;
}

const ble_ams_media_info_t *ble_ams_get_media_info(void)
{
    return &s_media;
}

bool ble_ams_is_active(void)
{
    return s_active;
}
