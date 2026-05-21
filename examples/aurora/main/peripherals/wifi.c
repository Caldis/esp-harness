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

static const char *TAG = "wifi";

static bool s_inited = false;

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
