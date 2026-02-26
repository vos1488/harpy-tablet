/*
 * CarPlay-style Dashboard UI
 *
 * Full-screen car infotainment interface:
 * - BLE HID media controls (Play/Pause, Next, Prev, Vol+, Vol-)
 * - Connection status & pairing
 * - Clock display
 * - Now-playing-style layout
 *
 * Connects to phone via BLE HID (Consumer Control).
 * Phone sees "HARPY Remote" in Bluetooth settings.
 */

#include "ui_carplay.h"
#include "ui_home.h"
#include "ble_hid.h"
#include "ble_ams.h"
#include "ble_ancs.h"
#include "ble_nav_service.h"
#include "harpy_config.h"
#include "esp_log.h"
#include "time_manager.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <inttypes.h>

static const char *TAG = "ui_carplay";

/* ==================== Colors ==================== */
#define CP_BG         lv_color_hex(0x000000)
#define CP_PANEL      lv_color_hex(0x111111)
#define CP_ACCENT     lv_color_hex(0x0A84FF)  /* iOS blue */
#define CP_GREEN      lv_color_hex(0x30D158)
#define CP_ORANGE     lv_color_hex(0xFF9F0A)
#define CP_RED        lv_color_hex(0xFF453A)
#define CP_TEXT       lv_color_hex(0xFFFFFF)
#define CP_MUTED      lv_color_hex(0x8E8E93)
#define CP_BTN_BG     lv_color_hex(0x1C1C1E)

/* ==================== State ==================== */
static lv_obj_t *s_screen       = NULL;
static lv_obj_t *s_status_lbl   = NULL;
static lv_obj_t *s_peer_lbl     = NULL;
static lv_obj_t *s_play_btn     = NULL;
static lv_obj_t *s_play_icon    = NULL;
static lv_obj_t *s_time_lbl     = NULL;
static lv_obj_t *s_conn_btn     = NULL;
static lv_obj_t *s_conn_lbl     = NULL;
static lv_obj_t *s_title_lbl    = NULL;   /* Track title from AMS */
static lv_obj_t *s_artist_lbl   = NULL;   /* Artist - Album from AMS */
static lv_obj_t *s_bonds_lbl    = NULL;   /* Bonded devices count */
static lv_obj_t *s_nav_panel    = NULL;   /* Navigation overlay on right panel */
static lv_obj_t *s_dir_icon_lbl = NULL;   /* Direction arrow icon */
static lv_obj_t *s_dir_bg       = NULL;   /* Direction icon background circle */
static lv_obj_t *s_nav_instr_lbl = NULL;  /* Navigation instruction */
static lv_obj_t *s_nav_detail_lbl = NULL; /* Street/distance detail */
static lv_obj_t *s_nav_app_lbl  = NULL;   /* Source app name */
static lv_obj_t *s_tab_music    = NULL;   /* Music tab button */
static lv_obj_t *s_tab_nav      = NULL;   /* Navigation tab button */
static lv_obj_t *s_nav_dist_lbl  = NULL;   /* Distance to maneuver */
static lv_obj_t *s_nav_speed_lbl = NULL;   /* Current speed */
static lv_obj_t *s_nav_eta_lbl   = NULL;   /* ETA */
static lv_timer_t *s_update_timer = NULL;
static bool s_playing = false;

/* Passkey popup (car-stereo pairing display) */
static lv_obj_t *s_passkey_popup = NULL;
static volatile uint32_t s_passkey_value = 0;
static volatile bool s_passkey_pending = false;

/* AMS media update flag (thread-safe: set from NimBLE thread, read from LVGL) */
static volatile bool s_media_update_pending = false;

/* ANCS navigation update flag */
static volatile bool s_nav_update_pending = false;

/* Custom BLE Nav Service update flag */
static volatile bool s_custom_nav_update_pending = false;

/* Forward declarations */
static void dismiss_passkey_popup(void);
static void show_passkey_popup(uint32_t passkey);
static void carplay_passkey_cb(uint32_t passkey);
static void carplay_media_cb(const ble_ams_media_info_t *info);
static void carplay_nav_cb(const ble_ancs_nav_info_t *info);
static void carplay_custom_nav_cb(const ble_nav_data_t *data);
static void delete_bonds_cb(lv_event_t *e);
static void tab_music_cb(lv_event_t *e);
static void tab_nav_cb(lv_event_t *e);

/* ==================== Media Button Helpers ==================== */

static lv_obj_t *create_media_btn(lv_obj_t *parent, const char *icon,
                                   int w, int h, lv_color_t bg,
                                   lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, h / 2, 0);   /* Pill shape */
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    /* Press effect */
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, icon);
    lv_obj_set_style_text_color(lbl, CP_TEXT, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(lbl);

    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

/* ==================== Callbacks ==================== */

static void back_cb(lv_event_t *e)
{
    if (s_update_timer) {
        lv_timer_del(s_update_timer);
        s_update_timer = NULL;
    }
    dismiss_passkey_popup();
    ble_hid_set_passkey_cb(NULL);
    ble_ams_set_media_cb(NULL);
    ble_ancs_set_nav_cb(NULL);
    ble_nav_service_set_cb(NULL);
    lv_obj_t *scr = lv_event_get_user_data(e);
    lv_scr_load(ui_home_get_screen());
    if (scr) lv_obj_del(scr);
    s_screen = NULL;
    s_status_lbl = NULL;
    s_peer_lbl = NULL;
    s_play_btn = NULL;
    s_play_icon = NULL;
    s_time_lbl = NULL;
    s_conn_btn = NULL;
    s_conn_lbl = NULL;
    s_title_lbl = NULL;
    s_artist_lbl = NULL;
    s_bonds_lbl = NULL;
    s_nav_panel = NULL;
    s_dir_icon_lbl = NULL;
    s_dir_bg = NULL;
    s_nav_instr_lbl = NULL;
    s_nav_detail_lbl = NULL;
    s_nav_app_lbl = NULL;
    s_nav_dist_lbl = NULL;
    s_nav_speed_lbl = NULL;
    s_nav_eta_lbl = NULL;
    s_tab_music = NULL;
    s_tab_nav = NULL;
}

static void play_pause_cb(lv_event_t *e)
{
    ble_hid_play_pause();
    s_playing = !s_playing;
    if (s_play_icon) {
        lv_label_set_text(s_play_icon, s_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
}

static void next_cb(lv_event_t *e)    { ble_hid_next_track(); }
static void prev_cb(lv_event_t *e)    { ble_hid_prev_track(); }
static void vol_up_cb(lv_event_t *e)  { ble_hid_volume_up(); }
static void vol_down_cb(lv_event_t *e){ ble_hid_volume_down(); }
static void mute_cb(lv_event_t *e)    { ble_hid_mute(); }

static void connect_cb(lv_event_t *e)
{
    ble_hid_state_t st = ble_hid_get_state();
    if (st == BLE_HID_DISCONNECTED) {
        ble_hid_start_advertising();
    } else if (st == BLE_HID_ADVERTISING) {
        ble_hid_stop_advertising();
    }
}

/* Passkey callback — called from NimBLE thread, uses flag for LVGL thread safety */
static void carplay_passkey_cb(uint32_t passkey)
{
    s_passkey_value = passkey;
    s_passkey_pending = true;
    ESP_LOGI(TAG, "Passkey to display: %06" PRIu32, passkey);
}

/* AMS media callback — called from NimBLE thread */
static void carplay_media_cb(const ble_ams_media_info_t *info)
{
    s_media_update_pending = true;
    ESP_LOGI(TAG, "AMS update: %s - %s (%s)", info->artist, info->title,
             info->playing ? "playing" : "paused");
}

/* Delete all bonded devices */
static void delete_bonds_cb(lv_event_t *e)
{
    ble_hid_delete_all_bonds();
    ESP_LOGI(TAG, "All bonds deleted");
    if (s_bonds_lbl) {
        lv_label_set_text(s_bonds_lbl, "Bonds: 0 (cleared)");
    }
}

/* ANCS navigation callback — called from NimBLE thread */
static void carplay_nav_cb(const ble_ancs_nav_info_t *info)
{
    s_nav_update_pending = true;
    ESP_LOGI(TAG, "ANCS nav update: '%s' / '%s' (dir=%d, active=%d)",
             info->title, info->message, info->direction, info->active);
}

/* Custom BLE Nav Service callback — called from NimBLE thread */
static void carplay_custom_nav_cb(const ble_nav_data_t *data)
{
    s_custom_nav_update_pending = true;
    ESP_LOGI(TAG, "Custom nav: dir=%d dist='%s' instr='%s' street='%s' app='%s'",
             data->direction, data->distance, data->instruction,
             data->street, data->app_name);
}

/* Direction icon from enum */
static const char *dir_to_icon(nav_direction_t dir)
{
    switch (dir) {
    case NAV_DIR_STRAIGHT:     return LV_SYMBOL_UP;
    case NAV_DIR_LEFT:
    case NAV_DIR_SLIGHT_LEFT:  return LV_SYMBOL_LEFT;
    case NAV_DIR_RIGHT:
    case NAV_DIR_SLIGHT_RIGHT: return LV_SYMBOL_RIGHT;
    case NAV_DIR_UTURN:        return LV_SYMBOL_LOOP;
    case NAV_DIR_ARRIVE:       return LV_SYMBOL_HOME;
    case NAV_DIR_ROUNDABOUT:   return LV_SYMBOL_REFRESH;
    default:                   return LV_SYMBOL_GPS;
    }
}

/* Direction color from enum */
static lv_color_t dir_to_color(nav_direction_t dir)
{
    switch (dir) {
    case NAV_DIR_LEFT:
    case NAV_DIR_SLIGHT_LEFT:  return lv_color_hex(0xFF9F0A); /* orange */
    case NAV_DIR_RIGHT:
    case NAV_DIR_SLIGHT_RIGHT: return lv_color_hex(0x0A84FF); /* blue */
    case NAV_DIR_UTURN:        return lv_color_hex(0xFF453A); /* red */
    case NAV_DIR_ARRIVE:       return lv_color_hex(0x30D158); /* green */
    case NAV_DIR_ROUNDABOUT:   return lv_color_hex(0xBF5AF2); /* purple */
    default:                   return lv_color_hex(0x0A84FF); /* blue */
    }
}

/* Direction icon from custom nav direction enum */
static const char *custom_dir_to_icon(ble_nav_direction_t dir)
{
    switch (dir) {
    case BLE_NAV_DIR_STRAIGHT:     return LV_SYMBOL_UP;
    case BLE_NAV_DIR_LEFT:
    case BLE_NAV_DIR_SLIGHT_LEFT:  return LV_SYMBOL_LEFT;
    case BLE_NAV_DIR_RIGHT:
    case BLE_NAV_DIR_SLIGHT_RIGHT: return LV_SYMBOL_RIGHT;
    case BLE_NAV_DIR_UTURN:        return LV_SYMBOL_LOOP;
    case BLE_NAV_DIR_ARRIVE:       return LV_SYMBOL_HOME;
    case BLE_NAV_DIR_ROUNDABOUT:   return LV_SYMBOL_REFRESH;
    default:                       return LV_SYMBOL_GPS;
    }
}

/* Direction color from custom nav direction enum */
static lv_color_t custom_dir_to_color(ble_nav_direction_t dir)
{
    switch (dir) {
    case BLE_NAV_DIR_LEFT:
    case BLE_NAV_DIR_SLIGHT_LEFT:  return lv_color_hex(0xFF9F0A);
    case BLE_NAV_DIR_RIGHT:
    case BLE_NAV_DIR_SLIGHT_RIGHT: return lv_color_hex(0x0A84FF);
    case BLE_NAV_DIR_UTURN:        return lv_color_hex(0xFF453A);
    case BLE_NAV_DIR_ARRIVE:       return lv_color_hex(0x30D158);
    case BLE_NAV_DIR_ROUNDABOUT:   return lv_color_hex(0xBF5AF2);
    default:                       return lv_color_hex(0x0A84FF);
    }
}

/* Tab switching */
static void tab_music_cb(lv_event_t *e)
{
    if (s_nav_panel)  lv_obj_add_flag(s_nav_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_tab_music)  lv_obj_set_style_bg_color(s_tab_music, CP_ACCENT, 0);
    if (s_tab_nav)    lv_obj_set_style_bg_color(s_tab_nav, CP_BTN_BG, 0);
}

static void tab_nav_cb(lv_event_t *e)
{
    if (s_nav_panel)  lv_obj_clear_flag(s_nav_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_tab_music)  lv_obj_set_style_bg_color(s_tab_music, CP_BTN_BG, 0);
    if (s_tab_nav)    lv_obj_set_style_bg_color(s_tab_nav, CP_ACCENT, 0);
}

static void dismiss_passkey_popup(void)
{
    if (s_passkey_popup) {
        lv_obj_del(s_passkey_popup);
        s_passkey_popup = NULL;
    }
}

static void show_passkey_popup(uint32_t passkey)
{
    dismiss_passkey_popup();
    if (!s_screen) return;

    /* Full-screen semi-transparent overlay */
    s_passkey_popup = lv_obj_create(s_screen);
    lv_obj_set_size(s_passkey_popup, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(s_passkey_popup, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_passkey_popup, LV_OPA_80, 0);
    lv_obj_align(s_passkey_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_passkey_popup, LV_OBJ_FLAG_SCROLLABLE);

    /* Card */
    lv_obj_t *card = lv_obj_create(s_passkey_popup);
    lv_obj_set_size(card, 400, 260);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x0A84FF), 0);
    lv_obj_set_style_shadow_width(card, 30, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x0A84FF), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* BT icon */
    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x0A84FF), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 20);

    /* Title */
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Pairing Code");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 65);

    /* Passkey digits */
    char code_str[16];
    snprintf(code_str, sizeof(code_str), "%06" PRIu32, passkey);
    lv_obj_t *code_lbl = lv_label_create(card);
    lv_label_set_text(code_lbl, code_str);
    lv_obj_set_style_text_font(code_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(code_lbl, lv_color_hex(0x30D158), 0);
    lv_obj_set_style_text_letter_space(code_lbl, 12, 0);
    lv_obj_align(code_lbl, LV_ALIGN_CENTER, 0, 10);

    /* Hint */
    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint, "Enter this code on your phone");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8E8E93), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    ESP_LOGI(TAG, "Passkey popup shown: %s", code_str);
}

/* ==================== Periodic Update ==================== */

static void update_timer_cb(lv_timer_t *timer)
{
    /* Check for passkey popup request (thread-safe: check volatile flag) */
    if (s_passkey_pending) {
        s_passkey_pending = false;
        show_passkey_popup(s_passkey_value);
    }

    /* Auto-dismiss passkey popup when paired or disconnected */
    if (s_passkey_popup) {
        ble_hid_state_t st = ble_hid_get_state();
        if (st == BLE_HID_DISCONNECTED) {
            dismiss_passkey_popup();
        }
    }

    /* AMS track info update (thread-safe: check volatile flag) */
    if (s_media_update_pending) {
        s_media_update_pending = false;
        const ble_ams_media_info_t *m = ble_ams_get_media_info();
        if (m) {
            if (s_title_lbl) {
                if (m->title[0])
                    lv_label_set_text(s_title_lbl, m->title);
                else
                    lv_label_set_text(s_title_lbl, "No Track");
            }
            if (s_artist_lbl) {
                char buf[270];
                if (m->artist[0] && m->album[0])
                    snprintf(buf, sizeof(buf), "%s — %s", m->artist, m->album);
                else if (m->artist[0])
                    snprintf(buf, sizeof(buf), "%s", m->artist);
                else if (m->album[0])
                    snprintf(buf, sizeof(buf), "%s", m->album);
                else
                    snprintf(buf, sizeof(buf), "Unknown Artist");
                lv_label_set_text(s_artist_lbl, buf);
            }
            /* Sync play/pause icon from AMS */
            if (s_play_icon) {
                s_playing = m->playing;
                lv_label_set_text(s_play_icon, s_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
            }
        }
    }

    /* Custom BLE Nav Service update (takes priority over ANCS) */
    if (s_custom_nav_update_pending) {
        s_custom_nav_update_pending = false;
        s_nav_update_pending = false;  /* custom overrides ANCS */
        const ble_nav_data_t *nav = ble_nav_service_get_data();
        if (nav && s_nav_panel) {
            if (nav->active) {
                /* Direction icon */
                if (s_dir_icon_lbl)
                    lv_label_set_text(s_dir_icon_lbl, custom_dir_to_icon(nav->direction));
                if (s_dir_bg)
                    lv_obj_set_style_bg_color(s_dir_bg, custom_dir_to_color(nav->direction), 0);

                /* Instruction */
                if (s_nav_instr_lbl)
                    lv_label_set_text(s_nav_instr_lbl,
                                     nav->instruction[0] ? nav->instruction : "Navigating...");

                /* Street + distance */
                if (s_nav_detail_lbl) {
                    char dbuf[300];
                    if (nav->street[0])
                        snprintf(dbuf, sizeof(dbuf), "%s", nav->street);
                    else
                        dbuf[0] = '\0';
                    lv_label_set_text(s_nav_detail_lbl, dbuf);
                }

                /* Distance to maneuver */
                if (s_nav_dist_lbl)
                    lv_label_set_text(s_nav_dist_lbl,
                                     nav->distance[0] ? nav->distance : "");

                /* Speed */
                if (s_nav_speed_lbl)
                    lv_label_set_text(s_nav_speed_lbl,
                                     nav->speed[0] ? nav->speed : "");

                /* ETA */
                if (s_nav_eta_lbl)
                    lv_label_set_text(s_nav_eta_lbl,
                                     nav->eta[0] ? nav->eta : "");

                /* App name */
                if (s_nav_app_lbl) {
                    char abuf[64];
                    snprintf(abuf, sizeof(abuf), "via %s", nav->app_name);
                    lv_label_set_text(s_nav_app_lbl, abuf);
                }

                /* Auto-switch to nav tab */
                if (s_nav_panel && lv_obj_has_flag(s_nav_panel, LV_OBJ_FLAG_HIDDEN)) {
                    tab_nav_cb(NULL);
                }
            } else {
                /* Navigation ended */
                if (s_dir_icon_lbl)
                    lv_label_set_text(s_dir_icon_lbl, LV_SYMBOL_GPS);
                if (s_dir_bg)
                    lv_obj_set_style_bg_color(s_dir_bg, CP_MUTED, 0);
                if (s_nav_instr_lbl)
                    lv_label_set_text(s_nav_instr_lbl, "No active navigation");
                if (s_nav_detail_lbl)
                    lv_label_set_text(s_nav_detail_lbl,
                                     "Install NavRelay tweak\non your iPhone");
                if (s_nav_dist_lbl) lv_label_set_text(s_nav_dist_lbl, "");
                if (s_nav_speed_lbl) lv_label_set_text(s_nav_speed_lbl, "");
                if (s_nav_eta_lbl) lv_label_set_text(s_nav_eta_lbl, "");
                if (s_nav_app_lbl) lv_label_set_text(s_nav_app_lbl, "");
            }
        }
    }

    /* ANCS navigation update (fallback if custom service not used) */
    if (s_nav_update_pending) {
        s_nav_update_pending = false;
        const ble_ancs_nav_info_t *nav = ble_ancs_get_nav_info();
        if (nav && s_nav_panel) {
            if (nav->active) {
                if (s_dir_icon_lbl)
                    lv_label_set_text(s_dir_icon_lbl, dir_to_icon(nav->direction));
                if (s_dir_bg)
                    lv_obj_set_style_bg_color(s_dir_bg, dir_to_color(nav->direction), 0);
                if (s_nav_instr_lbl)
                    lv_label_set_text(s_nav_instr_lbl,
                                     nav->title[0] ? nav->title : "Navigating...");
                if (s_nav_detail_lbl) {
                    char dbuf[400];
                    if (nav->message[0])
                        snprintf(dbuf, sizeof(dbuf), "%s", nav->message);
                    else
                        dbuf[0] = '\0';
                    lv_label_set_text(s_nav_detail_lbl, dbuf);
                }
                if (s_nav_app_lbl) {
                    char abuf[64];
                    snprintf(abuf, sizeof(abuf), "via %s", nav->app_name);
                    lv_label_set_text(s_nav_app_lbl, abuf);
                }
                if (s_nav_panel && lv_obj_has_flag(s_nav_panel, LV_OBJ_FLAG_HIDDEN))
                    tab_nav_cb(NULL);
            }
        }
    }

    /* Connection status */
    if (s_status_lbl) {
        ble_hid_state_t st = ble_hid_get_state();
        const char *txt;
        lv_color_t clr;
        switch (st) {
            case BLE_HID_CONNECTED:
                txt = LV_SYMBOL_BLUETOOTH " Connected";
                clr = CP_GREEN;
                break;
            case BLE_HID_ADVERTISING:
                txt = LV_SYMBOL_BLUETOOTH " Waiting for device...";
                clr = CP_ORANGE;
                break;
            default:
                txt = LV_SYMBOL_BLUETOOTH " Disconnected";
                clr = CP_MUTED;
                break;
        }
        lv_label_set_text(s_status_lbl, txt);
        lv_obj_set_style_text_color(s_status_lbl, clr, 0);
    }

    /* Peer name */
    if (s_peer_lbl) {
        const char *peer = ble_hid_get_peer_name();
        if (peer && peer[0]) {
            char buf[48];
            snprintf(buf, sizeof(buf), "Device: %s", peer);
            lv_label_set_text(s_peer_lbl, buf);
        } else {
            lv_label_set_text(s_peer_lbl, "No device paired");
        }
    }

    /* Connect button label */
    if (s_conn_lbl) {
        ble_hid_state_t st = ble_hid_get_state();
        if (st == BLE_HID_CONNECTED) {
            lv_label_set_text(s_conn_lbl, LV_SYMBOL_OK " Paired");
        } else if (st == BLE_HID_ADVERTISING) {
            lv_label_set_text(s_conn_lbl, LV_SYMBOL_REFRESH " Searching...");
        } else {
            lv_label_set_text(s_conn_lbl, "Pair Device");
        }
    }

    /* Time */
    if (s_time_lbl) {
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        lv_label_set_text(s_time_lbl, buf);
    }
}

/* ==================== Screen Creation ==================== */

void ui_carplay_open(void)
{
    /* Ensure HID is initialized */
    ble_hid_init();

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, CP_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    /* ===== Top Bar ===== */
    lv_obj_t *top = lv_obj_create(s_screen);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LCD_H_RES, 44);
    lv_obj_set_style_bg_color(top, CP_PANEL, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);

    /* Back */
    lv_obj_t *btn_back = lv_btn_create(top);
    lv_obj_set_size(btn_back, 80, 36);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_radius(btn_back, 10, 0);
    lv_obj_add_event_cb(btn_back, back_cb, LV_EVENT_CLICKED, s_screen);
    lv_obj_t *bl = lv_label_create(btn_back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(bl, CP_ACCENT, 0);
    lv_obj_center(bl);

    /* Title */
    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, "CarPlay");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, CP_TEXT, 0);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, -60, 0);

    /* Tab buttons: Music | Nav */
    s_tab_music = lv_btn_create(top);
    lv_obj_set_size(s_tab_music, 72, 32);
    lv_obj_align(s_tab_music, LV_ALIGN_CENTER, 40, 0);
    lv_obj_set_style_bg_color(s_tab_music, CP_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_tab_music, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_tab_music, 8, 0);
    lv_obj_set_style_shadow_width(s_tab_music, 0, 0);
    lv_obj_set_style_border_width(s_tab_music, 0, 0);
    lv_obj_add_event_cb(s_tab_music, tab_music_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tm_lbl = lv_label_create(s_tab_music);
    lv_label_set_text(tm_lbl, LV_SYMBOL_AUDIO " Music");
    lv_obj_set_style_text_color(tm_lbl, CP_TEXT, 0);
    lv_obj_set_style_text_font(tm_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(tm_lbl);

    s_tab_nav = lv_btn_create(top);
    lv_obj_set_size(s_tab_nav, 72, 32);
    lv_obj_align(s_tab_nav, LV_ALIGN_CENTER, 120, 0);
    lv_obj_set_style_bg_color(s_tab_nav, CP_BTN_BG, 0);
    lv_obj_set_style_bg_opa(s_tab_nav, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_tab_nav, 8, 0);
    lv_obj_set_style_shadow_width(s_tab_nav, 0, 0);
    lv_obj_set_style_border_width(s_tab_nav, 0, 0);
    lv_obj_add_event_cb(s_tab_nav, tab_nav_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tn_lbl = lv_label_create(s_tab_nav);
    lv_label_set_text(tn_lbl, LV_SYMBOL_GPS " Nav");
    lv_obj_set_style_text_color(tn_lbl, CP_TEXT, 0);
    lv_obj_set_style_text_font(tn_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(tn_lbl);

    /* Time */
    s_time_lbl = lv_label_create(top);
    lv_label_set_text(s_time_lbl, "--:--");
    lv_obj_set_style_text_font(s_time_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_time_lbl, CP_TEXT, 0);
    lv_obj_align(s_time_lbl, LV_ALIGN_RIGHT_MID, -16, 0);

    /* ===== Main Content ===== */
    lv_obj_t *main_area = lv_obj_create(s_screen);
    lv_obj_remove_style_all(main_area);
    lv_obj_set_size(main_area, LCD_H_RES, LCD_V_RES - 44);
    lv_obj_align(main_area, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(main_area, LV_OBJ_FLAG_SCROLLABLE);

    /* ------- Left panel: Connection & Info ------- */
    lv_obj_t *left = lv_obj_create(main_area);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, 300, LCD_V_RES - 44);
    lv_obj_align(left, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_pad_all(left, 24, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(left, 16, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    /* Bluetooth icon (large) */
    lv_obj_t *bt_icon_bg = lv_obj_create(left);
    lv_obj_remove_style_all(bt_icon_bg);
    lv_obj_set_size(bt_icon_bg, 80, 80);
    lv_obj_set_style_bg_color(bt_icon_bg, CP_ACCENT, 0);
    lv_obj_set_style_bg_opa(bt_icon_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bt_icon_bg, 20, 0);
    lv_obj_clear_flag(bt_icon_bg, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *bt_icon = lv_label_create(bt_icon_bg);
    lv_label_set_text(bt_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_color(bt_icon, CP_TEXT, 0);
    lv_obj_set_style_text_font(bt_icon, &lv_font_montserrat_36, 0);
    lv_obj_center(bt_icon);

    /* Status */
    s_status_lbl = lv_label_create(left);
    lv_label_set_text(s_status_lbl, LV_SYMBOL_BLUETOOTH " Disconnected");
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_status_lbl, CP_MUTED, 0);

    /* Peer name */
    s_peer_lbl = lv_label_create(left);
    lv_label_set_text(s_peer_lbl, "No device paired");
    lv_obj_set_style_text_font(s_peer_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_peer_lbl, CP_MUTED, 0);

    /* Connect/Pair button */
    s_conn_btn = lv_btn_create(left);
    lv_obj_set_size(s_conn_btn, 220, 48);
    lv_obj_set_style_bg_color(s_conn_btn, CP_ACCENT, 0);
    lv_obj_set_style_radius(s_conn_btn, 12, 0);
    lv_obj_add_event_cb(s_conn_btn, connect_cb, LV_EVENT_CLICKED, NULL);
    s_conn_lbl = lv_label_create(s_conn_btn);
    lv_label_set_text(s_conn_lbl, "Pair Device");
    lv_obj_set_style_text_color(s_conn_lbl, CP_TEXT, 0);
    lv_obj_set_style_text_font(s_conn_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(s_conn_lbl);

    /* Hint */
    lv_obj_t *hint = lv_label_create(left);
    lv_label_set_text(hint,
        "1. Pair \"HARPY Remote\"\n"
        "   in BT settings\n"
        "2. Enter pairing code\n"
        "3. Control music here");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(hint, 240);

    /* Bonded devices info */
    s_bonds_lbl = lv_label_create(left);
    {
        int cnt = ble_hid_get_bonded_count();
        char bbuf[48];
        snprintf(bbuf, sizeof(bbuf), "Saved devices: %d", cnt);
        lv_label_set_text(s_bonds_lbl, bbuf);
    }
    lv_obj_set_style_text_font(s_bonds_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_bonds_lbl, CP_MUTED, 0);

    /* Delete bonds button */
    lv_obj_t *del_btn = lv_btn_create(left);
    lv_obj_set_size(del_btn, 180, 36);
    lv_obj_set_style_bg_color(del_btn, CP_RED, 0);
    lv_obj_set_style_bg_opa(del_btn, LV_OPA_70, 0);
    lv_obj_set_style_radius(del_btn, 10, 0);
    lv_obj_add_event_cb(del_btn, delete_bonds_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *del_lbl = lv_label_create(del_btn);
    lv_label_set_text(del_lbl, LV_SYMBOL_TRASH " Clear Bonds");
    lv_obj_set_style_text_color(del_lbl, CP_TEXT, 0);
    lv_obj_set_style_text_font(del_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(del_lbl);

    /* ------- Right panel: Media Controls ------- */
    lv_obj_t *right = lv_obj_create(main_area);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, 500, LCD_V_RES - 44);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(right, CP_PANEL, 0);
    lv_obj_set_style_bg_opa(right, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(right, 24, LV_PART_MAIN);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    /* "Now Playing" label */
    lv_obj_t *np_lbl = lv_label_create(right);
    lv_label_set_text(np_lbl, "Now Playing");
    lv_obj_set_style_text_font(np_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(np_lbl, CP_MUTED, 0);
    lv_obj_align(np_lbl, LV_ALIGN_TOP_MID, 0, 16);

    /* Track title (from AMS) */
    s_title_lbl = lv_label_create(right);
    lv_label_set_text(s_title_lbl, "No Track");
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_title_lbl, CP_TEXT, 0);
    lv_label_set_long_mode(s_title_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_title_lbl, 440);
    lv_obj_set_style_text_align(s_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_title_lbl, LV_ALIGN_TOP_MID, 0, 44);

    /* Artist — Album (from AMS) */
    s_artist_lbl = lv_label_create(right);
    lv_label_set_text(s_artist_lbl, "Connect & play music");
    lv_obj_set_style_text_font(s_artist_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_artist_lbl, CP_MUTED, 0);
    lv_label_set_long_mode(s_artist_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_artist_lbl, 440);
    lv_obj_set_style_text_align(s_artist_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_artist_lbl, LV_ALIGN_TOP_MID, 0, 76);

    /* ===== Album Art Placeholder (decorative circle) ===== */
    lv_obj_t *art = lv_obj_create(right);
    lv_obj_remove_style_all(art);
    lv_obj_set_size(art, 140, 140);
    lv_obj_set_style_bg_color(art, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(art, 70, 0);
    lv_obj_set_style_border_width(art, 3, 0);
    lv_obj_set_style_border_color(art, CP_ACCENT, 0);
    lv_obj_set_style_border_opa(art, LV_OPA_60, 0);
    lv_obj_align(art, LV_ALIGN_CENTER, 0, -20);
    lv_obj_clear_flag(art, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *music_icon = lv_label_create(art);
    lv_label_set_text(music_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(music_icon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(music_icon, CP_ACCENT, 0);
    lv_obj_center(music_icon);

    /* ===== Transport Controls Row ===== */
    lv_obj_t *ctrl_row = lv_obj_create(right);
    lv_obj_remove_style_all(ctrl_row);
    lv_obj_set_size(ctrl_row, 400, 80);
    lv_obj_align(ctrl_row, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ctrl_row, LV_OBJ_FLAG_SCROLLABLE);

    /* Prev */
    create_media_btn(ctrl_row, LV_SYMBOL_PREV, 60, 60, CP_BTN_BG, prev_cb);

    /* Play/Pause (larger) */
    s_play_btn = create_media_btn(ctrl_row, "", 76, 76, CP_ACCENT, play_pause_cb);
    s_play_icon = lv_obj_get_child(s_play_btn, 0);
    lv_label_set_text(s_play_icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(s_play_icon, &lv_font_montserrat_36, 0);

    /* Next */
    create_media_btn(ctrl_row, LV_SYMBOL_NEXT, 60, 60, CP_BTN_BG, next_cb);

    /* ===== Volume Row ===== */
    lv_obj_t *vol_row = lv_obj_create(right);
    lv_obj_remove_style_all(vol_row);
    lv_obj_set_size(vol_row, 300, 52);
    lv_obj_align(vol_row, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_flex_flow(vol_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);

    create_media_btn(vol_row, LV_SYMBOL_VOLUME_MID, 50, 44, CP_BTN_BG, vol_down_cb);

    /* Mute */
    create_media_btn(vol_row, LV_SYMBOL_MUTE, 50, 44, CP_RED, mute_cb);

    create_media_btn(vol_row, LV_SYMBOL_VOLUME_MAX, 50, 44, CP_BTN_BG, vol_up_cb);

    /* ===== Navigation Panel (overlays right panel, hidden by default) ===== */
    s_nav_panel = lv_obj_create(main_area);
    lv_obj_remove_style_all(s_nav_panel);
    lv_obj_set_size(s_nav_panel, 500, LCD_V_RES - 44);
    lv_obj_align(s_nav_panel, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_nav_panel, CP_PANEL, 0);
    lv_obj_set_style_bg_opa(s_nav_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_nav_panel, 24, LV_PART_MAIN);
    lv_obj_clear_flag(s_nav_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_nav_panel, LV_OBJ_FLAG_HIDDEN);

    /* Nav header */
    lv_obj_t *nav_hdr = lv_label_create(s_nav_panel);
    lv_label_set_text(nav_hdr, "Navigation");
    lv_obj_set_style_text_font(nav_hdr, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(nav_hdr, CP_MUTED, 0);
    lv_obj_align(nav_hdr, LV_ALIGN_TOP_MID, 0, 16);

    /* Direction icon — large circle with arrow */
    s_dir_bg = lv_obj_create(s_nav_panel);
    lv_obj_remove_style_all(s_dir_bg);
    lv_obj_set_size(s_dir_bg, 140, 140);
    lv_obj_set_style_bg_color(s_dir_bg, CP_MUTED, 0);
    lv_obj_set_style_bg_opa(s_dir_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_dir_bg, 70, 0);
    lv_obj_set_style_border_width(s_dir_bg, 3, 0);
    lv_obj_set_style_border_color(s_dir_bg, CP_TEXT, 0);
    lv_obj_set_style_border_opa(s_dir_bg, LV_OPA_30, 0);
    lv_obj_align(s_dir_bg, LV_ALIGN_CENTER, 0, -50);
    lv_obj_clear_flag(s_dir_bg, LV_OBJ_FLAG_CLICKABLE);

    s_dir_icon_lbl = lv_label_create(s_dir_bg);
    lv_label_set_text(s_dir_icon_lbl, LV_SYMBOL_GPS);
    lv_obj_set_style_text_font(s_dir_icon_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_dir_icon_lbl, CP_TEXT, 0);
    lv_obj_center(s_dir_icon_lbl);

    /* Instruction label (large) */
    s_nav_instr_lbl = lv_label_create(s_nav_panel);
    lv_label_set_text(s_nav_instr_lbl, "No active navigation");
    lv_obj_set_style_text_font(s_nav_instr_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_nav_instr_lbl, CP_TEXT, 0);
    lv_obj_set_style_text_align(s_nav_instr_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_nav_instr_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_nav_instr_lbl, 440);
    lv_obj_align(s_nav_instr_lbl, LV_ALIGN_CENTER, 0, 40);

    /* Distance to maneuver (big text next to direction icon) */
    s_nav_dist_lbl = lv_label_create(s_nav_panel);
    lv_label_set_text(s_nav_dist_lbl, "");
    lv_obj_set_style_text_font(s_nav_dist_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_nav_dist_lbl, CP_TEXT, 0);
    lv_obj_align(s_nav_dist_lbl, LV_ALIGN_CENTER, 120, -50);

    /* Detail label (street name) */
    s_nav_detail_lbl = lv_label_create(s_nav_panel);
    lv_label_set_text(s_nav_detail_lbl,
                      "Install NavRelay tweak\non your iPhone");
    lv_obj_set_style_text_font(s_nav_detail_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_nav_detail_lbl, CP_MUTED, 0);
    lv_obj_set_style_text_align(s_nav_detail_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_nav_detail_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_nav_detail_lbl, 440);
    lv_obj_align(s_nav_detail_lbl, LV_ALIGN_CENTER, 0, 80);

    /* Speed + ETA row at bottom */
    lv_obj_t *info_row = lv_obj_create(s_nav_panel);
    lv_obj_remove_style_all(info_row);
    lv_obj_set_size(info_row, 440, 40);
    lv_obj_align(info_row, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_set_flex_flow(info_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(info_row, LV_OBJ_FLAG_SCROLLABLE);

    /* Speed label */
    s_nav_speed_lbl = lv_label_create(info_row);
    lv_label_set_text(s_nav_speed_lbl, "");
    lv_obj_set_style_text_font(s_nav_speed_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_nav_speed_lbl, CP_GREEN, 0);

    /* ETA label */
    s_nav_eta_lbl = lv_label_create(info_row);
    lv_label_set_text(s_nav_eta_lbl, "");
    lv_obj_set_style_text_font(s_nav_eta_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_nav_eta_lbl, CP_ACCENT, 0);

    /* Source app label */
    s_nav_app_lbl = lv_label_create(s_nav_panel);
    lv_label_set_text(s_nav_app_lbl, "");
    lv_obj_set_style_text_font(s_nav_app_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_nav_app_lbl, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_align(s_nav_app_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_nav_app_lbl, LV_ALIGN_BOTTOM_MID, 0, -20);

    /* Supported apps hint */
    lv_obj_t *apps_hint = lv_label_create(s_nav_panel);
    lv_label_set_text(apps_hint,
        LV_SYMBOL_GPS "  NavRelay tweak required (jailbreak)");
    lv_obj_set_style_text_font(apps_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(apps_hint, lv_color_hex(0x444444), 0);
    lv_obj_align(apps_hint, LV_ALIGN_BOTTOM_MID, 0, -35);

    /* ===== Update Timer ===== */
    s_update_timer = lv_timer_create(update_timer_cb, 500, NULL);

    /* Register passkey display callback (car-stereo pairing) */
    ble_hid_set_passkey_cb(carplay_passkey_cb);

    /* Register AMS media callback for track info updates */
    ble_ams_set_media_cb(carplay_media_cb);

    /* Register ANCS navigation callback for notifications */
    ble_ancs_set_nav_cb(carplay_nav_cb);

    /* Register custom BLE Nav Service callback for turn-by-turn */
    ble_nav_service_set_cb(carplay_custom_nav_cb);

    lv_scr_load(s_screen);
    ESP_LOGI(TAG, "CarPlay screen opened (media + nav service)");
}
