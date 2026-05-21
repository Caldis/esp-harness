/*
 * wifi_creds.h — persistent WiFi credentials (NVS-backed).
 *
 * Separates the "where do credentials live" concern from the "WiFi
 * radio control" concern (wifi.c). Credentials live in NVS namespace
 * "wifi_cred" with keys "ssid" and "pass". Both are bounded by ESP-IDF
 * constants (32 bytes SSID, 64 bytes pass).
 *
 * Security notes:
 *   - NVS by default is plaintext-on-flash. For shipped products,
 *     enable NVS encryption via menuconfig + a key in the flash
 *     encryption keyset. We keep the API the same either way.
 *   - The plaintext PASS is never returned via `?wifi status` (only
 *     "configured: true/false" is exposed).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_CRED_SSID_MAX  32
#define WIFI_CRED_PASS_MAX  64

/* Initialise (open the namespace). Safe to call multiple times. */
bool wifi_creds_init(void);

/* Persist a new (ssid, pass) pair. `pass` may be NULL for open networks
 * (treated as empty string). Returns true on success. */
bool wifi_creds_set(const char *ssid, const char *pass);

/* Read into caller-provided buffers. `out_ssid` and `out_pass` must each
 * be at least WIFI_CRED_*_MAX + 1 bytes. Returns true if credentials
 * exist and were read into the buffers; false if no credentials are
 * stored. */
bool wifi_creds_get(char *out_ssid, size_t ssid_cap,
                    char *out_pass, size_t pass_cap);

/* True if credentials are currently stored (just checks SSID presence). */
bool wifi_creds_has(void);

/* Erase stored credentials. Returns true on success (or "already absent"). */
bool wifi_creds_forget(void);

#ifdef __cplusplus
}
#endif
