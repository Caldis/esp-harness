/*
 * wifi_creds.c — NVS-backed WiFi credential storage.
 *
 * The NVS namespace "wifi_cred" holds two string keys: "ssid" and
 * "pass". We don't lazy-allocate or cache — read directly from NVS on
 * every call. Credentials change rarely (once at provisioning), so the
 * latency of a sync NVS read is fine.
 *
 * nvs_flash_init() must have been called by app_main before this.
 */

#include "wifi_creds.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG       = "wifi_creds";
static const char *NVS_NS    = "wifi_cred";
static const char *K_SSID    = "ssid";
static const char *K_PASS    = "pass";

bool wifi_creds_init(void)
{
    /* Try opening read-only to verify the namespace is reachable.
     * Non-existent namespace is OK at this stage. */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        nvs_close(h);
        ESP_LOGI(TAG, "init: namespace '%s' present", NVS_NS);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "init: no creds stored (will create on first set)");
    } else {
        ESP_LOGW(TAG, "init: nvs_open: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool wifi_creds_set(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGW(TAG, "set: empty SSID rejected");
        return false;
    }
    if (strlen(ssid) > WIFI_CRED_SSID_MAX) {
        ESP_LOGW(TAG, "set: SSID > %d chars rejected", WIFI_CRED_SSID_MAX);
        return false;
    }
    if (pass && strlen(pass) > WIFI_CRED_PASS_MAX) {
        ESP_LOGW(TAG, "set: PASS > %d chars rejected", WIFI_CRED_PASS_MAX);
        return false;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set: nvs_open: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_str(h, K_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, K_PASS, pass ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "set: stored ssid='%s' (pass %zu bytes)", ssid, pass ? strlen(pass) : 0);
        return true;
    }
    ESP_LOGW(TAG, "set: write failed: %s", esp_err_to_name(err));
    return false;
}

bool wifi_creds_get(char *out_ssid, size_t ssid_cap,
                    char *out_pass, size_t pass_cap)
{
    if (!out_ssid || ssid_cap == 0 || !out_pass || pass_cap == 0) return false;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    size_t s_len = ssid_cap;
    esp_err_t err = nvs_get_str(h, K_SSID, out_ssid, &s_len);
    if (err != ESP_OK) {
        nvs_close(h);
        return false;
    }

    size_t p_len = pass_cap;
    err = nvs_get_str(h, K_PASS, out_pass, &p_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* SSID present but PASS missing — treat as open network. */
        out_pass[0] = '\0';
    } else if (err != ESP_OK) {
        nvs_close(h);
        return false;
    }
    nvs_close(h);
    return true;
}

bool wifi_creds_has(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, K_SSID, NULL, &len);
    nvs_close(h);
    return err == ESP_OK && len > 1;   /* len includes NUL */
}

bool wifi_creds_forget(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;  /* nothing to forget */
    if (err != ESP_OK) return false;

    nvs_erase_key(h, K_SSID);   /* ignore "not found" */
    nvs_erase_key(h, K_PASS);
    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "forget: cleared");
        return true;
    }
    ESP_LOGW(TAG, "forget: commit failed: %s", esp_err_to_name(err));
    return false;
}
