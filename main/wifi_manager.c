/*
 * WiFi Manager - handles scanning, connecting, credentials storage
 */

#include "wifi_manager.h"
#include "harpy_config.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "wifi_mgr";

static wifi_state_t s_state = WIFI_STATE_DISCONNECTED;
static wifi_state_cb_t s_state_cb = NULL;
static wifi_scan_cb_t s_scan_cb = NULL;
static char s_ip_str[16] = "0.0.0.0";
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static int s_retry_count = 0;
static bool s_ap_active = false;
static char s_ap_ip_str[16] = "192.168.4.1";

/* ==================== Event Handlers ==================== */

static void set_state(wifi_state_t state)
{
    s_state = state;
    if (s_state_cb) {
        s_state_cb(state);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA started");
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGI(TAG, "Disconnected");
                s_ip_str[0] = '\0';
                if (s_state == WIFI_STATE_CONNECTING && s_retry_count < WIFI_MAX_RETRY) {
                    s_retry_count++;
                    ESP_LOGI(TAG, "Retry %d/%d", s_retry_count, WIFI_MAX_RETRY);
                    esp_wifi_connect();
                } else {
                    set_state(s_retry_count >= WIFI_MAX_RETRY ? WIFI_STATE_FAILED : WIFI_STATE_DISCONNECTED);
                }
                break;
            case WIFI_EVENT_SCAN_DONE: {
                uint16_t ap_count = 0;
                esp_wifi_scan_get_ap_num(&ap_count);
                if (ap_count > WIFI_SCAN_MAX_AP) ap_count = WIFI_SCAN_MAX_AP;

                wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
                if (ap_records && esp_wifi_scan_get_ap_records(&ap_count, ap_records) == ESP_OK) {
                    wifi_ap_info_t *ap_list = calloc(ap_count, sizeof(wifi_ap_info_t));
                    if (ap_list) {
                        for (int i = 0; i < ap_count; i++) {
                            strncpy(ap_list[i].ssid, (char *)ap_records[i].ssid, WIFI_SSID_MAX_LEN - 1);
                            ap_list[i].rssi = ap_records[i].rssi;
                            ap_list[i].auth = ap_records[i].authmode;
                        }
                        ESP_LOGI(TAG, "Scan done: %d APs found", ap_count);
                        if (s_scan_cb) {
                            s_scan_cb(ap_list, ap_count);
                        }
                        free(ap_list);
                    }
                }
                free(ap_records);
                break;
            }
            default:
                break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip_str);
        s_retry_count = 0;
        set_state(WIFI_STATE_CONNECTED);
    }
}

/* ==================== Public API ==================== */

void wifi_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi");

    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void wifi_manager_scan(wifi_scan_cb_t callback)
{
    s_scan_cb = callback;
    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };
    ESP_LOGI(TAG, "Starting scan...");
    esp_wifi_scan_start(&scan_cfg, false);
}

void wifi_manager_connect(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Connecting to: %s", ssid);
    s_retry_count = 0;
    set_state(WIFI_STATE_CONNECTING);

    /* Disconnect first if connected */
    esp_wifi_disconnect();

    wifi_config_t wifi_cfg = {};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (password && strlen(password) > 0) {
        strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    }
    wifi_cfg.sta.threshold.authmode = (password && strlen(password) > 0) 
        ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    esp_wifi_connect();
}

void wifi_manager_disconnect(void)
{
    esp_wifi_disconnect();
    set_state(WIFI_STATE_DISCONNECTED);
}

wifi_state_t wifi_manager_get_state(void)
{
    return s_state;
}

const char *wifi_manager_get_ip(void)
{
    return s_ip_str;
}

void wifi_manager_set_state_cb(wifi_state_cb_t cb)
{
    s_state_cb = cb;
}

void wifi_manager_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, "ssid", ssid);
        nvs_set_str(handle, "pass", password ? password : "");
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "Credentials saved for: %s", ssid);
    }
}

void wifi_manager_auto_connect(void)
{
    nvs_handle_t handle;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        char ssid[WIFI_SSID_MAX_LEN] = {};
        char pass[WIFI_PASS_MAX_LEN] = {};
        size_t ssid_len = sizeof(ssid);
        size_t pass_len = sizeof(pass);

        if (nvs_get_str(handle, "ssid", ssid, &ssid_len) == ESP_OK &&
            nvs_get_str(handle, "pass", pass, &pass_len) == ESP_OK &&
            strlen(ssid) > 0) {
            ESP_LOGI(TAG, "Auto-connecting to: %s", ssid);
            nvs_close(handle);
            wifi_manager_connect(ssid, pass);
            return;
        }
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "No saved credentials");
}

/* ==================== AP Mode ==================== */

void wifi_manager_start_ap(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Starting SoftAP: %s", ssid);

    /* Switch to APSTA mode so STA still works */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t ap_cfg = {};
    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(ssid);
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.max_connection = 4;

    if (password && strlen(password) >= 8) {
        strncpy((char *)ap_cfg.ap.password, password, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    s_ap_active = true;

    /* Get AP IP */
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        snprintf(s_ap_ip_str, sizeof(s_ap_ip_str), IPSTR, IP2STR(&ip_info.ip));
    }

    ESP_LOGI(TAG, "SoftAP started, IP: %s", s_ap_ip_str);
}

void wifi_manager_stop_ap(void)
{
    ESP_LOGI(TAG, "Stopping SoftAP");
    s_ap_active = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
}

bool wifi_manager_ap_is_active(void)
{
    return s_ap_active;
}

const char *wifi_manager_get_ap_ip(void)
{
    return s_ap_ip_str;
}

int wifi_manager_get_ap_sta_count(void)
{
    if (!s_ap_active) return 0;
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        return sta_list.num;
    }
    return 0;
}

/* ==================== Network Info ==================== */

void wifi_manager_get_mac(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int wifi_manager_get_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

void wifi_manager_get_gateway(char *buf, size_t len)
{
    if (!s_sta_netif) { buf[0] = '\0'; return; }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
        snprintf(buf, len, IPSTR, IP2STR(&ip_info.gw));
    } else {
        buf[0] = '\0';
    }
}

void wifi_manager_get_dns(char *buf, size_t len)
{
    if (!s_sta_netif) { buf[0] = '\0'; return; }
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        snprintf(buf, len, IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    } else {
        buf[0] = '\0';
    }
}

void wifi_manager_get_ssid(char *buf, size_t len)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        strncpy(buf, (char *)ap.ssid, len - 1);
        buf[len - 1] = '\0';
    } else {
        buf[0] = '\0';
    }
}
