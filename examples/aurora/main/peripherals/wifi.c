/*
 * wifi.c — minimal STA-mode scan.
 *
 * Lifecycle:
 *   wifi_init()  — NVS + esp_netif + event loop + esp_wifi_init +
 *                  set STA + start. Idempotent.
 *   wifi_scan()  — blocking, default scan config, copies top-N by RSSI
 *                  into the caller's buffer.
 *
 * We deliberately keep the AP scan list small (WIFI_SCAN_MAX_RESULTS=20):
 * the AXP2101 board has plenty of memory but a flat 50+ AP list
 * doesn't serve the AI loop — top 5–10 by RSSI is the demonstrative
 * subset.
 */

#include "wifi.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "wifi";

static bool s_inited = false;

/* Connect-state tracked via an event group + cached status. */
#define WIFI_BIT_CONNECTED  BIT0
#define WIFI_BIT_FAIL       BIT1
static EventGroupHandle_t      s_evt_group = NULL;
static esp_event_handler_instance_t s_h_wifi  = NULL;
static esp_event_handler_instance_t s_h_ip    = NULL;
static volatile bool           s_connected     = false;
static int                     s_retry         = 0;
#define MAX_CONNECT_RETRIES    3
/* Distinguishes a user-requested disconnect (wifi_disconnect / forget)
 * from an unsolicited link drop. The disconnect-event handler must
 * suppress its 3-retry auto-reconnect when this flag is set, otherwise
 * the device immediately re-associates and the user's intent is lost. */
static volatile bool           s_user_disconnect = false;
static char                    s_active_ssid[33] = {0};
static volatile int8_t         s_last_rssi   = 0;
static volatile uint32_t       s_ip_addr     = 0;
static volatile uint32_t       s_gw_addr     = 0;
static volatile uint32_t       s_netmask     = 0;

static esp_err_t nvs_init_once(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

bool wifi_init(void)
{
    if (s_inited) return true;

    /* NVS / esp_netif / event loop / default-wifi-sta-netif must be done
     * by aurora_main's early-init (step 0). We assume that's been called
     * already — checking netif existence costs a syscall and isn't worth
     * it. If the caller didn't run early-init, esp_wifi_init below will
     * surface a clear error. */
    if (nvs_init_once() != ESP_OK) return false;

    /* Create the default STA netif once. If it already exists, just look
     * it up — esp_netif_get_handle_from_ifkey is idempotent. */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == NULL) {
        sta = esp_netif_create_default_wifi_sta();
        if (sta == NULL) {
            ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
            return false;
        }
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err;
    if ((err = esp_wifi_init(&init)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return false;
    }
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) {
        ESP_LOGE(TAG, "set_storage: %s", esp_err_to_name(err));
        return false;
    }
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) {
        ESP_LOGE(TAG, "set_mode: %s", esp_err_to_name(err));
        return false;
    }
    if ((err = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(TAG, "wifi_start: %s", esp_err_to_name(err));
        return false;
    }

    s_inited = true;
    ESP_LOGI(TAG, "wifi STA started");
    return true;
}

static int cmp_rssi_desc(const void *a, const void *b)
{
    const wifi_ap_t *x = (const wifi_ap_t *)a;
    const wifi_ap_t *y = (const wifi_ap_t *)b;
    if (y->rssi != x->rssi) return (int)y->rssi - (int)x->rssi;
    return 0;
}

int wifi_scan(wifi_ap_t *out, int max_out, int timeout_ms)
{
    if (!s_inited) {
        if (!wifi_init()) return -1;
    }
    if (out == NULL || max_out <= 0) return -1;

    wifi_scan_config_t cfg = {
        .ssid         = NULL,
        .bssid        = NULL,
        .channel      = 0,
        .show_hidden  = false,
        .scan_type    = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 80,
                .max = (uint32_t)(timeout_ms > 0 ? timeout_ms : 600),
            },
        },
    };
    esp_err_t err = esp_wifi_scan_start(&cfg, true /* block */);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan_start: %s", esp_err_to_name(err));
        return -1;
    }

    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK) return -1;
    if (count == 0) return 0;

    /* Cap at our scratch buffer size for sorting. */
    uint16_t want = count > WIFI_SCAN_MAX_RESULTS ? WIFI_SCAN_MAX_RESULTS : count;
    wifi_ap_record_t *records = (wifi_ap_record_t *)calloc(want, sizeof(*records));
    if (records == NULL) return -1;
    uint16_t got = want;
    if (esp_wifi_scan_get_ap_records(&got, records) != ESP_OK) {
        free(records);
        return -1;
    }

    /* Copy into our trim struct, sort by RSSI desc. */
    int copied = got < max_out ? got : max_out;
    for (int i = 0; i < copied; ++i) {
        memset(&out[i], 0, sizeof(out[i]));
        strlcpy(out[i].ssid, (const char *)records[i].ssid, sizeof(out[i].ssid));
        out[i].rssi     = records[i].rssi;
        out[i].channel  = records[i].primary;
        out[i].authmode = (uint8_t)records[i].authmode;
        memcpy(out[i].bssid, records[i].bssid, 6);
    }
    free(records);
    qsort(out, copied, sizeof(out[0]), cmp_rssi_desc);
    return copied;
}

const char *wifi_auth_label(uint8_t authmode)
{
    switch (authmode) {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:             return "wep";
        case WIFI_AUTH_WPA_PSK:         return "wpa";
        case WIFI_AUTH_WPA2_PSK:        return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "wpa/wpa2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2-ent";
        case WIFI_AUTH_WPA3_PSK:        return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa2/wpa3";
        default:                         return "?";
    }
}

/* ── connect-mode implementation ───────────────────────────────────── */

static void wifi_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != WIFI_EVENT) return;
    switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_connected = false;
            s_ip_addr   = 0;
            s_gw_addr   = 0;
            s_netmask   = 0;
            if (s_user_disconnect) {
                ESP_LOGI(TAG, "disconnect → user-requested, not retrying");
                /* Leave s_user_disconnect set; cleared on next connect. */
            } else if (s_retry < MAX_CONNECT_RETRIES) {
                s_retry++;
                ESP_LOGI(TAG, "disconnect → retry %d/%d", s_retry, MAX_CONNECT_RETRIES);
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "disconnect → max retries reached");
                if (s_evt_group) xEventGroupSetBits(s_evt_group, WIFI_BIT_FAIL);
            }
            break;
        default:
            break;
    }
}

static void ip_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != IP_EVENT || id != IP_EVENT_STA_GOT_IP) return;
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    s_connected = true;
    s_retry     = 0;
    s_ip_addr   = e->ip_info.ip.addr;
    s_gw_addr   = e->ip_info.gw.addr;
    s_netmask   = e->ip_info.netmask.addr;
    ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
    if (s_evt_group) xEventGroupSetBits(s_evt_group, WIFI_BIT_CONNECTED);
}

static bool ensure_event_handlers(void)
{
    if (s_evt_group != NULL) return true;
    s_evt_group = xEventGroupCreate();
    if (!s_evt_group) return false;
    esp_err_t err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_cb, NULL, &s_h_wifi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register WIFI_EVENT: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_cb, NULL, &s_h_ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register IP_EVENT: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool wifi_connect(const char *ssid, const char *pass, int timeout_ms)
{
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGW(TAG, "connect: empty SSID");
        return false;
    }
    if (!wifi_init()) return false;
    if (!ensure_event_handlers()) return false;

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    if (pass) {
        strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    }
    /* Open-network-friendly default; let the AP choose stronger if it
     * advertises. */
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable    = true;
    cfg.sta.pmf_cfg.required   = false;

    /* Stash for status reporting BEFORE we kick the radio. */
    strlcpy(s_active_ssid, ssid, sizeof(s_active_ssid));
    s_retry            = 0;
    s_connected        = false;
    s_user_disconnect  = false;  /* fresh attempt — re-enable auto-retry */
    xEventGroupClearBits(s_evt_group, WIFI_BIT_CONNECTED | WIFI_BIT_FAIL);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "wifi_connect: %s", esp_err_to_name(err));
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_evt_group,
        WIFI_BIT_CONNECTED | WIFI_BIT_FAIL,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : 10000));
    return (bits & WIFI_BIT_CONNECTED) != 0;
}

bool wifi_disconnect(void)
{
    if (!s_inited) return true;
    /* Set the user-intent flag BEFORE calling esp_wifi_disconnect so
     * the event handler (runs on the wifi-event task) sees it when the
     * disconnect event fires, and skips the auto-retry path. */
    s_user_disconnect = true;
    esp_err_t err = esp_wifi_disconnect();
    s_connected = false;
    s_ip_addr   = 0;
    s_gw_addr   = 0;
    s_netmask   = 0;
    s_active_ssid[0] = '\0';
    return err == ESP_OK;
}

bool wifi_is_connected(void) { return s_connected; }

void wifi_get_status(wifi_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    /* "configured" is wifi_creds_has(); we don't link to wifi_creds.h
     * here to keep the layers separable — callers check it themselves
     * and fill the field. So we just leave configured=false; the
     * console-side cmd merges in the answer from wifi_creds. */
    out->connected = s_connected;
    if (s_connected) {
        strlcpy(out->ssid, s_active_ssid, sizeof(out->ssid));
        /* refresh RSSI cheaply via esp_wifi_sta_get_ap_info */
        wifi_ap_record_t info;
        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
            s_last_rssi = info.rssi;
        }
        out->rssi    = s_last_rssi;
        out->ip_addr = s_ip_addr;
        out->gw_addr = s_gw_addr;
        out->netmask = s_netmask;
    }
}
