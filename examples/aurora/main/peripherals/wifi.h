/*
 * wifi.h — WiFi STA scan helper.
 *
 * Aurora's "touch WiFi" milestone: prove the radio works by scanning
 * for nearby access points. We don't auto-connect to anything (that
 * would need credentials), but `?wifi scan` returns a JSON snapshot
 * with the AP count and a top-N by RSSI summary.
 *
 * NVS must be initialised before wifi_init() — wifi_init does it
 * lazily if not already done.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SCAN_MAX_RESULTS  20

typedef struct {
    char     ssid[33];     /* SSID, NUL-terminated (max 32 bytes + NUL) */
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  authmode;     /* WIFI_AUTH_OPEN / WPA2 / ... */
    uint8_t  bssid[6];
} wifi_ap_t;

bool wifi_init(void);

/* Synchronous scan. Returns the count of APs (0..WIFI_SCAN_MAX_RESULTS),
 * sorted by RSSI desc. Caller-provided buffer is filled in-place.
 * Negative on error. */
int  wifi_scan(wifi_ap_t *out, int max_out, int timeout_ms);

/* Short auth-mode label for JSON ("open", "wpa2", "wpa3", etc). */
const char *wifi_auth_label(uint8_t authmode);

/* ── Station-mode connect / status (v1.7+) ─────────────────────────
 *
 * The connect path lives separately from scan because it needs an
 * event-loop subscriber + an IP-acquisition gate. After wifi_connect
 * succeeds, the device is on the network until wifi_disconnect or
 * reboot.
 */

typedef struct {
    bool      configured;        /* credentials present (whether or not connected) */
    bool      connected;         /* link up + IP acquired */
    char      ssid[33];          /* currently-connected SSID (empty if not) */
    int8_t    rssi;              /* last reported RSSI, 0 if not connected */
    uint32_t  ip_addr;           /* IPv4 little-endian (0 if not connected) */
    uint32_t  gw_addr;
    uint32_t  netmask;
} wifi_status_t;

/* Connect to (ssid, pass). Blocks up to timeout_ms waiting for IP_GOT.
 * `pass` may be NULL or "" for open networks. Returns true on
 * acquisition of IP. */
bool wifi_connect(const char *ssid, const char *pass, int timeout_ms);

/* Disconnect from the current AP. Returns true on clean stop. */
bool wifi_disconnect(void);

/* True if currently link-up + IP acquired. */
bool wifi_is_connected(void);

/* Read status snapshot into caller-provided struct. */
void wifi_get_status(wifi_status_t *out);

#ifdef __cplusplus
}
#endif
