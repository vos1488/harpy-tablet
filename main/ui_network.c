/*
 * Network Tools Screen
 * Combined panel for: WiFi AP Hotspot, HTTP File Server,
 * HTTP File Downloader, and detailed Network Info.
 */

#include "ui_network.h"
#include "ui_home.h"
#include "harpy_config.h"
#include "wifi_manager.h"
#include "file_server.h"
#include "ui_sdcard.h"
#include "ui_keyboard.h"

#include "esp_http_client.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_net";

static lv_obj_t *s_screen = NULL;
static lv_timer_t *s_info_timer = NULL;

/* Network info labels */
static lv_obj_t *s_ssid_lbl = NULL;
static lv_obj_t *s_ip_lbl = NULL;
static lv_obj_t *s_gw_lbl = NULL;
static lv_obj_t *s_dns_lbl = NULL;
static lv_obj_t *s_mac_lbl = NULL;
static lv_obj_t *s_rssi_lbl = NULL;

/* AP controls */
static lv_obj_t *s_ap_status_lbl = NULL;
static lv_obj_t *s_ap_btn_lbl = NULL;
static lv_obj_t *s_ap_btn = NULL;
static lv_obj_t *s_ap_clients_lbl = NULL;

/* File server controls */
static lv_obj_t *s_srv_status_lbl = NULL;
static lv_obj_t *s_srv_btn = NULL;
static lv_obj_t *s_srv_btn_lbl = NULL;
static lv_obj_t *s_srv_req_lbl = NULL;

/* Download controls */
static lv_obj_t *s_dl_status_lbl = NULL;
static lv_obj_t *s_dl_bar = NULL;

static char s_download_url[256] = "";
static volatile bool s_downloading = false;

/* ==================== Info Refresh Timer ==================== */

static void info_refresh_cb(lv_timer_t *timer)
{
    /* Network info */
    if (wifi_manager_get_state() == WIFI_STATE_CONNECTED) {
        char buf[64];
        if (s_ssid_lbl) {
            wifi_manager_get_ssid(buf, sizeof(buf));
            lv_label_set_text_fmt(s_ssid_lbl, "SSID: %s", buf);
        }
        if (s_ip_lbl) {
            lv_label_set_text_fmt(s_ip_lbl, "IP: %s", wifi_manager_get_ip());
        }
        if (s_gw_lbl) {
            wifi_manager_get_gateway(buf, sizeof(buf));
            lv_label_set_text_fmt(s_gw_lbl, "GW: %s", buf);
        }
        if (s_dns_lbl) {
            wifi_manager_get_dns(buf, sizeof(buf));
            lv_label_set_text_fmt(s_dns_lbl, "DNS: %s", buf);
        }
        if (s_rssi_lbl) {
            int rssi = wifi_manager_get_rssi();
            const char *quality = rssi > -50 ? "Excellent" :
                                  rssi > -65 ? "Good" :
                                  rssi > -75 ? "Fair" : "Weak";
            lv_label_set_text_fmt(s_rssi_lbl, "RSSI: %d dBm (%s)", rssi, quality);
        }
    } else {
        if (s_ssid_lbl) lv_label_set_text(s_ssid_lbl, "SSID: Not connected");
        if (s_ip_lbl)   lv_label_set_text(s_ip_lbl, "IP: ---");
        if (s_gw_lbl)   lv_label_set_text(s_gw_lbl, "GW: ---");
        if (s_dns_lbl)  lv_label_set_text(s_dns_lbl, "DNS: ---");
        if (s_rssi_lbl) lv_label_set_text(s_rssi_lbl, "RSSI: ---");
    }

    if (s_mac_lbl) {
        char mac[20];
        wifi_manager_get_mac(mac, sizeof(mac));
        lv_label_set_text_fmt(s_mac_lbl, "MAC: %s", mac);
    }

    /* AP status */
    if (s_ap_status_lbl) {
        if (wifi_manager_ap_is_active()) {
            lv_label_set_text_fmt(s_ap_status_lbl, "AP: ON  IP: %s",
                                  wifi_manager_get_ap_ip());
            if (s_ap_clients_lbl) {
                lv_label_set_text_fmt(s_ap_clients_lbl, "Clients: %d",
                                      wifi_manager_get_ap_sta_count());
            }
        } else {
            lv_label_set_text(s_ap_status_lbl, "AP: OFF");
            if (s_ap_clients_lbl) lv_label_set_text(s_ap_clients_lbl, "");
        }
    }

    /* Server status */
    if (s_srv_status_lbl) {
        if (file_server_is_running()) {
            const char *ip = wifi_manager_ap_is_active() ?
                             wifi_manager_get_ap_ip() : wifi_manager_get_ip();
            lv_label_set_text_fmt(s_srv_status_lbl, "Server: http://%s:80", ip);
        } else {
            lv_label_set_text(s_srv_status_lbl, "Server: Stopped");
        }
    }
    if (s_srv_req_lbl && file_server_is_running()) {
        lv_label_set_text_fmt(s_srv_req_lbl, "Requests: %lu",
                              (unsigned long)file_server_get_request_count());
    }
}

/* ==================== AP Hotspot Callbacks ==================== */

static void ap_toggle_cb(lv_event_t *e)
{
    if (wifi_manager_ap_is_active()) {
        wifi_manager_stop_ap();
        if (s_ap_btn_lbl) lv_label_set_text(s_ap_btn_lbl, LV_SYMBOL_WIFI " Start AP");
        if (s_ap_btn) lv_obj_set_style_bg_color(s_ap_btn, HARPY_COLOR_SUCCESS, 0);
    } else {
        wifi_manager_start_ap("HARPY-AP", "harpy1234");
        if (s_ap_btn_lbl) lv_label_set_text(s_ap_btn_lbl, LV_SYMBOL_CLOSE " Stop AP");
        if (s_ap_btn) lv_obj_set_style_bg_color(s_ap_btn, HARPY_COLOR_ERROR, 0);
    }
}

/* ==================== File Server Callbacks ==================== */

static void server_toggle_cb(lv_event_t *e)
{
    if (file_server_is_running()) {
        file_server_stop();
        if (s_srv_btn_lbl) lv_label_set_text(s_srv_btn_lbl, LV_SYMBOL_UPLOAD " Start Server");
        if (s_srv_btn) lv_obj_set_style_bg_color(s_srv_btn, HARPY_COLOR_SUCCESS, 0);
    } else {
        if (!sd_is_mounted()) {
            /* Try to mount */
            if (!sd_mount()) {
                lv_label_set_text(s_srv_status_lbl, "Server: No SD card!");
                return;
            }
        }
        file_server_start(80);
        if (s_srv_btn_lbl) lv_label_set_text(s_srv_btn_lbl, LV_SYMBOL_CLOSE " Stop Server");
        if (s_srv_btn) lv_obj_set_style_bg_color(s_srv_btn, HARPY_COLOR_ERROR, 0);
    }
}

/* ==================== File Download ==================== */

static void download_task(void *arg)
{
    char *url = (char *)arg;
    if (!url) { s_downloading = false; vTaskDelete(NULL); return; }

    /* Extract filename from URL */
    const char *fname = strrchr(url, '/');
    fname = (fname && fname[1]) ? fname + 1 : "download.bin";

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", sd_get_mount_point(), fname);

    ESP_LOGI(TAG, "Downloading %s -> %s", url, filepath);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open fail: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(url);
        s_downloading = false;
        vTaskDelete(NULL);
        return;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int total = content_len > 0 ? content_len : 0;

    FILE *f = fopen(filepath, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create file: %s", filepath);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(url);
        s_downloading = false;
        vTaskDelete(NULL);
        return;
    }

    char buf[4096];
    int received = 0;
    int len;
    while ((len = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, len, f);
        received += len;
        /* Update progress */
        if (total > 0 && s_dl_bar) {
            int pct = (int)((int64_t)received * 100 / total);
            lv_bar_set_value(s_dl_bar, pct, LV_ANIM_ON);
        }
        if (s_dl_status_lbl) {
            char status[128];
            if (total > 0) {
                snprintf(status, sizeof(status), "Downloading: %d / %d KB",
                         received / 1024, total / 1024);
            } else {
                snprintf(status, sizeof(status), "Downloading: %d KB", received / 1024);
            }
            lv_label_set_text(s_dl_status_lbl, status);
        }
    }

    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(url);

    ESP_LOGI(TAG, "Download complete: %d bytes", received);
    if (s_dl_status_lbl) {
        char status[128];
        snprintf(status, sizeof(status), "Done: %s (%d KB)", fname, received / 1024);
        lv_label_set_text(s_dl_status_lbl, status);
    }
    if (s_dl_bar) lv_bar_set_value(s_dl_bar, 100, LV_ANIM_ON);
    s_downloading = false;
    vTaskDelete(NULL);
}

static void download_url_entered(const char *url)
{
    if (!url || strlen(url) == 0 || s_downloading) return;
    if (!sd_is_mounted()) {
        if (!sd_mount()) {
            if (s_dl_status_lbl) lv_label_set_text(s_dl_status_lbl, "No SD card!");
            return;
        }
    }

    strncpy(s_download_url, url, sizeof(s_download_url) - 1);
    s_downloading = true;
    if (s_dl_bar) lv_bar_set_value(s_dl_bar, 0, LV_ANIM_OFF);
    if (s_dl_status_lbl) lv_label_set_text(s_dl_status_lbl, "Starting download...");

    char *url_copy = strdup(url);
    xTaskCreatePinnedToCore(download_task, "download", 8192, url_copy, 4, NULL, 0);
}

static void download_btn_cb(lv_event_t *e)
{
    if (s_downloading) return;
    ui_keyboard_show(lv_scr_act(), "Enter URL", "http://", download_url_entered);
}

/* ==================== Screen Callbacks ==================== */

static void back_cb(lv_event_t *e)
{
    if (s_info_timer) { lv_timer_del(s_info_timer); s_info_timer = NULL; }
    s_ssid_lbl = s_ip_lbl = s_gw_lbl = s_dns_lbl = s_mac_lbl = s_rssi_lbl = NULL;
    s_ap_status_lbl = s_ap_btn_lbl = s_ap_btn = s_ap_clients_lbl = NULL;
    s_srv_status_lbl = s_srv_btn = s_srv_btn_lbl = s_srv_req_lbl = NULL;
    s_dl_status_lbl = NULL;
    s_dl_bar = NULL;
    lv_obj_t *scr = lv_event_get_user_data(e);
    lv_scr_load(ui_home_get_screen());
    if (scr) lv_obj_del(scr);
    s_screen = NULL;
}

/* ==================== Helper: Section Title ==================== */

static lv_obj_t *create_section_title(lv_obj_t *parent, const char *icon, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s  %s", icon, text);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl, HARPY_COLOR_PRIMARY, 0);
    return lbl;
}

static lv_obj_t *create_info_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, HARPY_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    return lbl;
}

static lv_obj_t *create_action_btn(lv_obj_t *parent, const char *text,
                                    lv_color_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, lv_pct(100), 40);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
    return btn;
}

/* ==================== Public API ==================== */

void ui_network_open(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, HARPY_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    /* Top bar */
    lv_obj_t *top = lv_obj_create(s_screen);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LCD_H_RES, 50);
    lv_obj_set_style_bg_color(top, HARPY_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *btn_back = lv_btn_create(top);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_bg_color(btn_back, HARPY_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(btn_back, 8, 0);
    lv_obj_add_event_cb(btn_back, back_cb, LV_EVENT_CLICKED, s_screen);
    lv_obj_t *lb = lv_label_create(btn_back);
    lv_label_set_text(lb, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(lb, lv_color_white(), 0);
    lv_obj_center(lb);

    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  Network Tools");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, HARPY_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* Content: 3 columns */
    lv_obj_t *content = lv_obj_create(s_screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, LCD_H_RES - 20, LCD_V_RES - 60);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(content, 8, 0);

    /* ===== Column 1: Network Info ===== */
    lv_obj_t *col1 = lv_obj_create(content);
    lv_obj_set_size(col1, 260, lv_pct(100));
    lv_obj_set_style_bg_color(col1, HARPY_COLOR_CARD, 0);
    lv_obj_set_style_radius(col1, 12, 0);
    lv_obj_set_style_border_width(col1, 0, 0);
    lv_obj_set_style_pad_all(col1, 12, 0);
    lv_obj_set_flex_flow(col1, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col1, 5, 0);

    create_section_title(col1, LV_SYMBOL_EYE_OPEN, "Network Info");
    s_ssid_lbl = create_info_label(col1, "SSID: ---");
    s_ip_lbl = create_info_label(col1, "IP: ---");
    s_gw_lbl = create_info_label(col1, "GW: ---");
    s_dns_lbl = create_info_label(col1, "DNS: ---");
    s_mac_lbl = create_info_label(col1, "MAC: ---");
    s_rssi_lbl = create_info_label(col1, "RSSI: ---");

    /* Divider */
    lv_obj_t *div1 = lv_obj_create(col1);
    lv_obj_remove_style_all(div1);
    lv_obj_set_size(div1, lv_pct(100), 1);
    lv_obj_set_style_bg_color(div1, HARPY_COLOR_MUTED, 0);
    lv_obj_set_style_bg_opa(div1, LV_OPA_30, 0);

    /* Download section in column 1 */
    create_section_title(col1, LV_SYMBOL_DOWNLOAD, "Download to SD");

    lv_obj_t *dl_btn = create_action_btn(col1, LV_SYMBOL_DOWNLOAD " Download URL",
                                          HARPY_COLOR_PRIMARY, download_btn_cb);
    (void)dl_btn;

    s_dl_bar = lv_bar_create(col1);
    lv_obj_set_size(s_dl_bar, lv_pct(100), 14);
    lv_bar_set_range(s_dl_bar, 0, 100);
    lv_bar_set_value(s_dl_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_dl_bar, HARPY_COLOR_BG, 0);
    lv_obj_set_style_bg_color(s_dl_bar, HARPY_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_dl_bar, 7, 0);
    lv_obj_set_style_radius(s_dl_bar, 7, LV_PART_INDICATOR);

    s_dl_status_lbl = create_info_label(col1, "Idle");

    /* ===== Column 2: AP Hotspot ===== */
    lv_obj_t *col2 = lv_obj_create(content);
    lv_obj_set_size(col2, 240, lv_pct(100));
    lv_obj_set_style_bg_color(col2, HARPY_COLOR_CARD, 0);
    lv_obj_set_style_radius(col2, 12, 0);
    lv_obj_set_style_border_width(col2, 0, 0);
    lv_obj_set_style_pad_all(col2, 12, 0);
    lv_obj_set_flex_flow(col2, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col2, 6, 0);

    create_section_title(col2, LV_SYMBOL_WIFI, "WiFi Hotspot");

    create_info_label(col2, "SSID: HARPY-AP");
    create_info_label(col2, "Pass: harpy1234");

    s_ap_btn = create_action_btn(col2,
        wifi_manager_ap_is_active() ? LV_SYMBOL_CLOSE " Stop AP" : LV_SYMBOL_WIFI " Start AP",
        wifi_manager_ap_is_active() ? HARPY_COLOR_ERROR : HARPY_COLOR_SUCCESS,
        ap_toggle_cb);
    /* Get the label inside the button */
    s_ap_btn_lbl = lv_obj_get_child(s_ap_btn, 0);

    s_ap_status_lbl = create_info_label(col2,
        wifi_manager_ap_is_active() ? "AP: ON" : "AP: OFF");
    s_ap_clients_lbl = create_info_label(col2, "");

    /* Divider */
    lv_obj_t *div2 = lv_obj_create(col2);
    lv_obj_remove_style_all(div2);
    lv_obj_set_size(div2, lv_pct(100), 1);
    lv_obj_set_style_bg_color(div2, HARPY_COLOR_MUTED, 0);
    lv_obj_set_style_bg_opa(div2, LV_OPA_30, 0);

    /* ===== File Server in Column 2 ===== */
    create_section_title(col2, LV_SYMBOL_UPLOAD, "File Server");

    s_srv_btn = create_action_btn(col2,
        file_server_is_running() ? LV_SYMBOL_CLOSE " Stop Server" : LV_SYMBOL_UPLOAD " Start Server",
        file_server_is_running() ? HARPY_COLOR_ERROR : HARPY_COLOR_SUCCESS,
        server_toggle_cb);
    s_srv_btn_lbl = lv_obj_get_child(s_srv_btn, 0);

    s_srv_status_lbl = create_info_label(col2, "Server: Stopped");
    s_srv_req_lbl = create_info_label(col2, "");

    /* ===== Column 3: Quick actions / Tips ===== */
    lv_obj_t *col3 = lv_obj_create(content);
    lv_obj_set_flex_grow(col3, 1);
    lv_obj_set_height(col3, lv_pct(100));
    lv_obj_set_style_bg_color(col3, HARPY_COLOR_CARD, 0);
    lv_obj_set_style_radius(col3, 12, 0);
    lv_obj_set_style_border_width(col3, 0, 0);
    lv_obj_set_style_pad_all(col3, 12, 0);
    lv_obj_set_flex_flow(col3, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col3, 6, 0);

    create_section_title(col3, LV_SYMBOL_LIST, "How to Use");

    lv_obj_t *tips = lv_label_create(col3);
    lv_label_set_text(tips,
        "1. WiFi Hotspot:\n"
        "   Start AP to create a WiFi\n"
        "   network. Connect from\n"
        "   phone/PC to HARPY-AP.\n\n"
        "2. File Server:\n"
        "   Start server, then open\n"
        "   browser on connected\n"
        "   device and go to the\n"
        "   shown URL. Browse,\n"
        "   upload, download files.\n\n"
        "3. Download:\n"
        "   Enter a URL to download\n"
        "   a file directly to SD card.\n"
        "   Needs WiFi STA connected.");
    lv_obj_set_style_text_color(tips, HARPY_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(tips, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(tips, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(tips, lv_pct(100));

    /* Start info refresh timer (1 second) */
    s_info_timer = lv_timer_create(info_refresh_cb, 1000, NULL);
    /* Immediate first refresh */
    info_refresh_cb(NULL);

    lv_scr_load(s_screen);
    ESP_LOGI(TAG, "Network tools opened");
}
