/*
 * Apple Notification Center Service (ANCS) Client
 *
 * Subscribes to iOS notifications, filters navigation app notifications,
 * and parses turn-by-turn instructions for display on the LCD.
 *
 * Discovery chain (async):
 *   1. Discover ANCS service by UUID
 *   2. Discover all characteristics (NS, CP, DS)
 *   3. Discover descriptors → find CCCDs
 *   4. Write Data Source CCCD (Apple requires DS before NS)
 *   5. Write Notification Source CCCD
 *   6. Receive notifications → request attributes → parse
 *
 * ANCS Service:         7905F431-B5CE-4E99-A40F-4B1E122D00D0
 * Notification Source:  9FBF120D-6301-42D9-8C58-25E699A21DBD
 * Control Point:        69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9
 * Data Source:          22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB
 */

#include "ble_ancs.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include <string.h>

static const char *TAG = "ble_ancs";

/* ==================== ANCS UUIDs (128-bit, little-endian for NimBLE) ==================== */

/* 7905F431-B5CE-4E99-A40F-4B1E122D00D0 */
static const ble_uuid128_t ancs_svc_uuid = BLE_UUID128_INIT(
    0xd0, 0x00, 0x2d, 0x12, 0x1e, 0x4b, 0x0f, 0xa4,
    0x99, 0x4e, 0xce, 0xb5, 0x31, 0xf4, 0x05, 0x79
);

/* 9FBF120D-6301-42D9-8C58-25E699A21DBD — Notification Source */
static const ble_uuid128_t ancs_ns_uuid = BLE_UUID128_INIT(
    0xbd, 0x1d, 0xa2, 0x99, 0xe6, 0x25, 0x58, 0x8c,
    0xd9, 0x42, 0x01, 0x63, 0x0d, 0x12, 0xbf, 0x9f
);

/* 69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9 — Control Point */
static const ble_uuid128_t ancs_cp_uuid = BLE_UUID128_INIT(
    0xd9, 0xd9, 0xaa, 0xfd, 0xbd, 0x9b, 0x21, 0x98,
    0xa8, 0x49, 0xe1, 0x45, 0xf3, 0xd8, 0xd1, 0x69
);

/* 22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB — Data Source */
static const ble_uuid128_t ancs_ds_uuid = BLE_UUID128_INIT(
    0xfb, 0x7b, 0x7c, 0xce, 0x6a, 0xb3, 0x44, 0xbe,
    0xb5, 0x4b, 0xd6, 0x24, 0xe9, 0xc6, 0xea, 0x22
);

/* ==================== State ==================== */
static uint16_t s_conn_handle;
static uint16_t s_svc_start;
static uint16_t s_svc_end;
static uint16_t s_ns_val_handle;    /* Notification Source */
static uint16_t s_cp_val_handle;    /* Control Point */
static uint16_t s_ds_val_handle;    /* Data Source */
static uint16_t s_ns_cccd_handle;
static uint16_t s_ds_cccd_handle;
static bool     s_discovered;

static ble_ancs_nav_info_t s_nav;
static ble_ancs_nav_cb_t   s_nav_cb;

/* Attribute request state */
static bool     s_request_pending;
static uint32_t s_current_uid;

/* Data Source reassembly buffer */
static uint8_t  s_ds_buf[512];
static uint16_t s_ds_len;

/* ==================== Navigation App Detection ==================== */

static bool is_nav_app(const char *app_id)
{
    if (!app_id || !app_id[0]) return false;
    return (strstr(app_id, "com.apple.Maps") != NULL ||
            strstr(app_id, "com.google.Maps") != NULL ||
            strstr(app_id, "com.waze") != NULL ||
            strstr(app_id, "yandex.navi") != NULL ||
            strstr(app_id, "yandex.traffic") != NULL ||
            strstr(app_id, "yandex.mobile.navigator") != NULL ||
            strstr(app_id, "2gis") != NULL);
}

/* ==================== Direction Parsing ==================== */

static nav_direction_t parse_direction(const char *text)
{
    if (!text || !text[0]) return NAV_DIR_NONE;

    /* Check longer/specific patterns first to avoid false matches */
    if (strstr(text, "U-turn") || strstr(text, "u-turn") ||
        strstr(text, "U-Turn") ||
        strstr(text, "\xd0\xa0\xd0\xb0\xd0\xb7\xd0\xb2\xd0\xbe\xd1\x80\xd0\xbe\xd1\x82"))  /* Разворот UTF-8 */
        return NAV_DIR_UTURN;

    if (strstr(text, "slight left") || strstr(text, "Slight left") ||
        strstr(text, "bear left") || strstr(text, "Bear left") ||
        strstr(text, "keep left") || strstr(text, "Keep left"))
        return NAV_DIR_SLIGHT_LEFT;

    if (strstr(text, "slight right") || strstr(text, "Slight right") ||
        strstr(text, "bear right") || strstr(text, "Bear right") ||
        strstr(text, "keep right") || strstr(text, "Keep right"))
        return NAV_DIR_SLIGHT_RIGHT;

    if (strstr(text, "left") || strstr(text, "Left") ||
        strstr(text, "\xd0\xbd\xd0\xb0\xd0\xbb\xd0\xb5\xd0\xb2\xd0\xbe") ||  /* налево */
        strstr(text, "\xd0\x9d\xd0\xb0\xd0\xbb\xd0\xb5\xd0\xb2\xd0\xbe"))    /* Налево */
        return NAV_DIR_LEFT;

    if (strstr(text, "right") || strstr(text, "Right") ||
        strstr(text, "\xd0\xbd\xd0\xb0\xd0\xbf\xd1\x80\xd0\xb0\xd0\xb2\xd0\xbe") ||  /* направо */
        strstr(text, "\xd0\x9d\xd0\xb0\xd0\xbf\xd1\x80\xd0\xb0\xd0\xb2\xd0\xbe"))    /* Направо */
        return NAV_DIR_RIGHT;

    if (strstr(text, "straight") || strstr(text, "Straight") ||
        strstr(text, "continue") || strstr(text, "Continue") ||
        strstr(text, "\xd0\xbf\xd1\x80\xd1\x8f\xd0\xbc\xd0\xbe") ||   /* прямо */
        strstr(text, "\xd0\x9f\xd1\x80\xd1\x8f\xd0\xbc\xd0\xbe"))    /* Прямо */
        return NAV_DIR_STRAIGHT;

    if (strstr(text, "arrive") || strstr(text, "Arrive") ||
        strstr(text, "destination") || strstr(text, "Destination") ||
        strstr(text, "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb1\xd1\x8b"))    /* прибы... */
        return NAV_DIR_ARRIVE;

    if (strstr(text, "roundabout") || strstr(text, "Roundabout") ||
        strstr(text, "\xd0\xba\xd0\xbe\xd0\xbb\xd1\x8c\xd1\x86") ||   /* кольц */
        strstr(text, "\xd0\x9a\xd0\xbe\xd0\xbb\xd1\x8c\xd1\x86"))    /* Кольц */
        return NAV_DIR_ROUNDABOUT;

    return NAV_DIR_NONE;
}

/* ==================== Request Notification Attributes ==================== */

static int cp_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        ESP_LOGW(TAG, "CP write response error: %d", error->status);
        s_request_pending = false;
    } else {
        ESP_LOGI(TAG, "CP write OK — waiting for Data Source response");
    }
    return 0;
}

static void request_attributes(uint32_t uid)
{
    if (!s_discovered || s_cp_val_handle == 0) return;

    /*
     * GetNotificationAttributes command:
     *   CommandID(1) = 0x00
     *   NotificationUID(4) LE
     *   AttrID_AppIdentifier(1) = 0x00
     *   AttrID_Title(1) = 0x01, MaxLen(2) = 64
     *   AttrID_Subtitle(1) = 0x02, MaxLen(2) = 64
     *   AttrID_Message(1) = 0x03, MaxLen(2) = 128
     */
    uint8_t cmd[] = {
        0x00,                                       /* CommandID: GetNotificationAttributes */
        (uint8_t)(uid),       (uint8_t)(uid >> 8),  /* UID LE */
        (uint8_t)(uid >> 16), (uint8_t)(uid >> 24),
        0x00,                                       /* AppIdentifier */
        0x01, 0x40, 0x00,                           /* Title, max 64 */
        0x02, 0x40, 0x00,                           /* Subtitle, max 64 */
        0x03, 0x80, 0x00,                           /* Message, max 128 */
    };

    /* ANCS Control Point requires Write With Response (not write_no_rsp!) */
    int rc = ble_gattc_write_flat(s_conn_handle, s_cp_val_handle,
                                   cmd, sizeof(cmd), cp_write_cb, NULL);
    if (rc == 0) {
        s_request_pending = true;
        s_current_uid = uid;
        s_ds_len = 0;
        ESP_LOGI(TAG, "Requested attrs for UID 0x%08lx", (unsigned long)uid);
    } else {
        ESP_LOGW(TAG, "CP write failed: %d", rc);
    }
}

/* ==================== Parse Data Source Response ==================== */

static bool try_parse_response(void)
{
    if (s_ds_len < 5) return false;
    if (s_ds_buf[0] != 0x00) return false;   /* Not GetNotificationAttributes */

    uint16_t pos = 5;   /* Skip CommandID(1) + NotificationUID(4) */

    char app_id[64]     = "";
    char title_text[128] = "";
    char subtitle[128]  = "";
    char message[256]   = "";
    int  attrs_done = 0;

    while (pos < s_ds_len && attrs_done < 4) {
        if (pos + 3 > s_ds_len) return false;   /* Need AttrID + Length */

        uint8_t  aid  = s_ds_buf[pos++];
        uint16_t alen = s_ds_buf[pos] | ((uint16_t)s_ds_buf[pos + 1] << 8);
        pos += 2;

        if (pos + alen > s_ds_len) return false; /* Need value bytes */

        switch (aid) {
        case 0x00:
            if (alen < sizeof(app_id)) {
                memcpy(app_id, &s_ds_buf[pos], alen);
                app_id[alen] = '\0';
            }
            break;
        case 0x01:
            if (alen < sizeof(title_text)) {
                memcpy(title_text, &s_ds_buf[pos], alen);
                title_text[alen] = '\0';
            }
            break;
        case 0x02:
            if (alen < sizeof(subtitle)) {
                memcpy(subtitle, &s_ds_buf[pos], alen);
                subtitle[alen] = '\0';
            }
            break;
        case 0x03:
            if (alen < sizeof(message)) {
                memcpy(message, &s_ds_buf[pos], alen);
                message[alen] = '\0';
            }
            break;
        }
        pos += alen;
        attrs_done++;
    }

    if (attrs_done < 4) return false;

    ESP_LOGI(TAG, "Parsed: app='%s' title='%s' sub='%s' msg='%s'",
             app_id, title_text, subtitle, message);

    /* Only update navigation if it's from a known navigation app */
    if (is_nav_app(app_id)) {
        strncpy(s_nav.title, title_text, sizeof(s_nav.title) - 1);
        strncpy(s_nav.subtitle, subtitle, sizeof(s_nav.subtitle) - 1);
        strncpy(s_nav.message, message, sizeof(s_nav.message) - 1);

        /* Extract short app name from bundle ID (last component) */
        const char *last_dot = strrchr(app_id, '.');
        strncpy(s_nav.app_name, last_dot ? last_dot + 1 : app_id,
                sizeof(s_nav.app_name) - 1);

        s_nav.direction = parse_direction(title_text);
        if (s_nav.direction == NAV_DIR_NONE)
            s_nav.direction = parse_direction(message);

        s_nav.active = true;

        ESP_LOGI(TAG, "NAV: dir=%d '%s' / '%s'", s_nav.direction,
                 s_nav.title, s_nav.message);
        if (s_nav_cb) s_nav_cb(&s_nav);
    }

    return true;
}

/* ==================== Discovery Chain ==================== */

/* Forward declarations */
static int ancs_svc_disc_cb(uint16_t, const struct ble_gatt_error *,
                            const struct ble_gatt_svc *, void *);
static int ancs_chr_disc_cb(uint16_t, const struct ble_gatt_error *,
                            const struct ble_gatt_chr *, void *);
static int ancs_dsc_disc_cb(uint16_t, const struct ble_gatt_error *,
                            uint16_t, const struct ble_gatt_dsc *, void *);
static int ancs_ds_cccd_cb(uint16_t, const struct ble_gatt_error *,
                           struct ble_gatt_attr *, void *);
static int ancs_ns_cccd_cb(uint16_t, const struct ble_gatt_error *,
                           struct ble_gatt_attr *, void *);

/* Step 5: Notification Source CCCD written — fully subscribed */
static int ancs_ns_cccd_cb(uint16_t conn, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0) {
        s_discovered = true;
        ESP_LOGI(TAG, "*** ANCS fully subscribed — ready for notifications ***");
    } else {
        ESP_LOGW(TAG, "NS CCCD write failed: %d", error->status);
    }
    return 0;
}

/* Step 4: Data Source CCCD written → subscribe Notification Source */
static int ancs_ds_cccd_cb(uint16_t conn, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0) {
        ESP_LOGI(TAG, "Data Source subscribed → subscribing Notification Source...");
        uint8_t val[2] = {0x01, 0x00};  /* Enable notifications */
        ble_gattc_write_flat(conn, s_ns_cccd_handle, val, 2, ancs_ns_cccd_cb, NULL);
    } else {
        ESP_LOGW(TAG, "DS CCCD write failed: %d", error->status);
    }
    return 0;
}

/* Step 3: Discover descriptors → find CCCDs → subscribe Data Source first */
static int ancs_dsc_disc_cb(uint16_t conn, const struct ble_gatt_error *error,
                            uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                            void *arg)
{
    if (error->status == 0 && dsc) {
        uint16_t uuid16 = ble_uuid_u16(&dsc->uuid.u);
        ESP_LOGI(TAG, "  DSC handle=%d uuid=0x%04x", dsc->handle, uuid16);
        if (uuid16 == 0x2902) {
            /*
             * Assign CCCD to the nearest characteristic above it.
             * Simple approach: whichever val_handle is closest below dsc->handle.
             */
            uint16_t best_chr = 0;
            if (s_ns_val_handle != 0 && dsc->handle > s_ns_val_handle)
                best_chr = s_ns_val_handle;
            if (s_ds_val_handle != 0 && dsc->handle > s_ds_val_handle &&
                s_ds_val_handle > best_chr)
                best_chr = s_ds_val_handle;

            if (best_chr == s_ns_val_handle && s_ns_cccd_handle == 0) {
                s_ns_cccd_handle = dsc->handle;
                ESP_LOGI(TAG, "  -> NS CCCD = %d", dsc->handle);
            } else if (best_chr == s_ds_val_handle && s_ds_cccd_handle == 0) {
                s_ds_cccd_handle = dsc->handle;
                ESP_LOGI(TAG, "  -> DS CCCD = %d", dsc->handle);
            } else {
                ESP_LOGI(TAG, "  -> CCCD %d (unassigned, best_chr=%d)", dsc->handle, best_chr);
            }
        }
    } else if (error->status == BLE_HS_EDONE) {
        /* Fallback: CCCD is typically val_handle + 1 */
        if (s_ds_cccd_handle == 0 && s_ds_val_handle != 0) {
            s_ds_cccd_handle = s_ds_val_handle + 1;
            ESP_LOGI(TAG, "Using fallback DS CCCD = %d", s_ds_cccd_handle);
        }
        if (s_ns_cccd_handle == 0 && s_ns_val_handle != 0) {
            s_ns_cccd_handle = s_ns_val_handle + 1;
            ESP_LOGI(TAG, "Using fallback NS CCCD = %d", s_ns_cccd_handle);
        }

        if (s_ds_cccd_handle == 0 || s_ns_cccd_handle == 0) {
            ESP_LOGW(TAG, "Could not find ANCS CCCDs (DS=%d NS=%d)",
                     s_ds_cccd_handle, s_ns_cccd_handle);
            return 0;
        }

        /* Apple requires DS to be subscribed BEFORE NS */
        ESP_LOGI(TAG, "Subscribing Data Source (CCCD %d)...", s_ds_cccd_handle);
        uint8_t val[2] = {0x01, 0x00};
        ble_gattc_write_flat(conn, s_ds_cccd_handle, val, 2, ancs_ds_cccd_cb, NULL);
    }
    return 0;
}

/* Step 2: Discover characteristics → find NS, CP, DS handles */
static int ancs_chr_disc_cb(uint16_t conn, const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0 && chr) {
        if (ble_uuid_cmp(&chr->uuid.u, &ancs_ns_uuid.u) == 0) {
            s_ns_val_handle = chr->val_handle;
            ESP_LOGI(TAG, "  Notification Source handle: %d", chr->val_handle);
        } else if (ble_uuid_cmp(&chr->uuid.u, &ancs_cp_uuid.u) == 0) {
            s_cp_val_handle = chr->val_handle;
            ESP_LOGI(TAG, "  Control Point handle: %d", chr->val_handle);
        } else if (ble_uuid_cmp(&chr->uuid.u, &ancs_ds_uuid.u) == 0) {
            s_ds_val_handle = chr->val_handle;
            ESP_LOGI(TAG, "  Data Source handle: %d", chr->val_handle);
        }
    } else if (error->status == BLE_HS_EDONE) {
        if (s_ns_val_handle == 0 || s_cp_val_handle == 0 || s_ds_val_handle == 0) {
            ESP_LOGW(TAG, "ANCS chars incomplete: NS=%d CP=%d DS=%d",
                     s_ns_val_handle, s_cp_val_handle, s_ds_val_handle);
            return 0;
        }
        ESP_LOGI(TAG, "All ANCS chars found — discovering descriptors...");

        /* Discover all DSCs in the service handle range */
        uint16_t start = s_ns_val_handle;
        if (s_cp_val_handle < start) start = s_cp_val_handle;
        if (s_ds_val_handle < start) start = s_ds_val_handle;

        ble_gattc_disc_all_dscs(conn, start, s_svc_end,
                                ancs_dsc_disc_cb, NULL);
    }
    return 0;
}

/* Step 1: Service found → discover characteristics */
static int ancs_svc_disc_cb(uint16_t conn, const struct ble_gatt_error *error,
                            const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status == 0 && svc) {
        s_svc_start = svc->start_handle;
        s_svc_end   = svc->end_handle;
        ESP_LOGI(TAG, "ANCS service found: handles %d-%d",
                 svc->start_handle, svc->end_handle);
    } else if (error->status == BLE_HS_EDONE) {
        if (s_svc_end == 0) {
            ESP_LOGW(TAG, "ANCS service not found on peer");
            return 0;
        }
        ESP_LOGI(TAG, "Discovering ANCS characteristics...");
        ble_gattc_disc_all_chrs(conn, s_svc_start, s_svc_end,
                                ancs_chr_disc_cb, NULL);
    }
    return 0;
}

/* ==================== Public API ==================== */

void ble_ancs_start_discovery(uint16_t conn_handle)
{
    s_conn_handle    = conn_handle;
    s_discovered     = false;
    s_ns_val_handle  = 0;
    s_cp_val_handle  = 0;
    s_ds_val_handle  = 0;
    s_ns_cccd_handle = 0;
    s_ds_cccd_handle = 0;
    s_svc_start      = 0;
    s_svc_end        = 0;
    s_request_pending = false;
    s_ds_len         = 0;

    ESP_LOGI(TAG, "Starting ANCS discovery (conn=%d)...", conn_handle);
    int rc = ble_gattc_disc_svc_by_uuid(conn_handle,
                                         (const ble_uuid_t *)&ancs_svc_uuid,
                                         ancs_svc_disc_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ANCS discovery start failed: %d", rc);
    }
}

void ble_ancs_on_disconnected(void)
{
    s_discovered = false;
    s_request_pending = false;
    s_ds_len = 0;
    memset(&s_nav, 0, sizeof(s_nav));
    ESP_LOGI(TAG, "ANCS disconnected, state cleared");
}

void ble_ancs_handle_notify(uint16_t attr_handle, const uint8_t *data, uint16_t len)
{
    if (!data || len == 0) return;

    if (attr_handle == s_ns_val_handle && s_discovered) {
        /* ===== Notification Source (8 bytes min) ===== */
        if (len < 8) return;

        uint8_t event_id    = data[0];   /* 0=Added, 1=Modified, 2=Removed */
        uint8_t event_flags = data[1];
        uint8_t cat_id      = data[2];   /* Category */
        /* uint8_t cat_cnt  = data[3]; */
        uint32_t uid = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                       ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);

        ESP_LOGI(TAG, "NS: event=%d flags=0x%02x cat=%d uid=0x%08lx",
                 event_id, event_flags, cat_id, (unsigned long)uid);

        if (event_id == 2) {
            /* Removed — clear navigation if this was our active UID */
            if (s_nav.active && s_current_uid == uid) {
                memset(&s_nav, 0, sizeof(s_nav));
                ESP_LOGI(TAG, "Navigation notification removed");
                if (s_nav_cb) s_nav_cb(&s_nav);
            }
            return;
        }

        /* Added (0) or Modified (1) — request attributes if not busy */
        if ((event_id == 0 || event_id == 1) && !s_request_pending) {
            request_attributes(uid);
        }

    } else if (attr_handle == s_ds_val_handle) {
        /* ===== Data Source — reassemble fragmented response ===== */
        if (s_ds_len + len < sizeof(s_ds_buf)) {
            memcpy(&s_ds_buf[s_ds_len], data, len);
            s_ds_len += len;

            if (try_parse_response()) {
                s_request_pending = false;
                s_ds_len = 0;
            }
        } else {
            ESP_LOGW(TAG, "DS buffer overflow (%d + %d), dropping", s_ds_len, len);
            s_request_pending = false;
            s_ds_len = 0;
        }
    }
}

void ble_ancs_set_nav_cb(ble_ancs_nav_cb_t cb)
{
    s_nav_cb = cb;
}

const ble_ancs_nav_info_t *ble_ancs_get_nav_info(void)
{
    return &s_nav;
}

bool ble_ancs_is_active(void)
{
    return s_discovered;
}
