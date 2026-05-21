/*
 * keys.h — three physical buttons on the Waveshare board.
 *
 * Per the Waveshare product wiki:
 *   BOOT  — strapping pin GPIO 0, also wired to a tactile button.
 *           Held low at boot enters download mode; after boot the same
 *           pin reads the button as input-pull-up (HIGH=released).
 *   USER  — GPIO 18, generic user button. Same active-low convention.
 *   PWR   — AXP2101 PWRON pin. Press events appear in the PMIC's IRQ
 *           status registers (we read short-press, long-press, and
 *           release flags from register 0x49 / 0x48).
 *
 * Threading: a background task polls at 20 Hz (50 ms) and caches the
 * state for the scene/console to read. PWR events are level-sensed
 * via the IRQ flag — the AXP2101 latches them until cleared, so a
 * single 50 ms tick won't miss a quick press.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool      boot_pressed;   /* GPIO 0 — active-low, currently held */
    bool      user_pressed;   /* GPIO 18 — active-low, currently held */
    bool      pwr_pressed;    /* AXP2101 PWRON — currently held */
    uint32_t  boot_count;     /* edge-counted presses since boot */
    uint32_t  user_count;
    uint32_t  pwr_count;
} keys_state_t;

bool keys_init(void);
void keys_get(keys_state_t *out);

#ifdef __cplusplus
}
#endif
