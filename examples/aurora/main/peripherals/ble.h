/*
 * ble.h — NimBLE-based BLE observer (scan-only).
 *
 * Aurora doesn't advertise, doesn't connect — just listens. The
 * milestone is "BLE radio is alive": kick off a passive scan for N ms
 * and report the number of advertisement events plus a sample of
 * unique device addresses with their strongest RSSI.
 *
 * Defaults to passive (no SCAN_REQ) for minimum airtime impact.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_SCAN_MAX_DEVICES   16

typedef struct {
    uint8_t  addr[6];
    int8_t   rssi;        /* strongest observed */
    uint8_t  addr_type;   /* random / public */
    char     name[20];    /* "" if no name advertised */
} ble_device_t;

bool ble_init(void);

/* Tear down the BLE stack so its internal-SRAM allocations can be
 * reclaimed (typically for WiFi to start). After this, ble_scan will
 * return -1; call ble_init() again to come back online. Returns true
 * on clean shutdown. */
bool ble_deinit(void);
bool ble_is_up(void);

/* Release the BT controller's reserved internal-SRAM pool *without*
 * having gone through ble_init first. Useful at boot when BLE was
 * never initialised but WiFi still needs the ~30 KB of contiguous
 * DRAM the controller keeps reserved by default. One-way — BLE can't
 * come back after this. Idempotent. */
bool ble_release_memory(void);

/* Blocking scan for `duration_ms`. Returns the number of unique
 * devices written into `out`, or negative on error.
 * `total_adv` (optional) is set to the raw advertisement-event count
 * for the AI to verify the radio is actively receiving. */
int  ble_scan(ble_device_t *out, int max_out, int duration_ms, int *total_adv);

#ifdef __cplusplus
}
#endif
