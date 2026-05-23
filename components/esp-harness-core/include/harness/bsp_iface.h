/*
 * harness/bsp_iface — the minimal BSP surface aurora-harness depends on.
 *
 * The component itself uses only two functions: the LVGL mutex
 * lock/unlock pair. Every Espressif-style BSP (`esp-bsp`, the various
 * Waveshare / M5Stack / LilyGO BSPs etc.) ships these with the same
 * signatures, so consuming projects pull in their BSP component as a
 * REQUIRES and these symbols resolve at link time.
 *
 * If you are bringing aurora-harness up on a board WITHOUT one of those
 * BSPs, simply define matching functions yourself. They typically wrap
 * a recursive FreeRTOS mutex that the LVGL task itself holds during
 * its callbacks. See sim/mock_bsp.{c,h} for the no-op stub used in the
 * host simulator build.
 *
 * See components/aurora-harness/PORTING.md for the full multi-board
 * walkthrough.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Acquire the LVGL mutex.
 *
 *   timeout_ms = -1   wait forever (the safe default for non-LVGL tasks
 *                     like console-command handlers)
 *   timeout_ms =  0   non-blocking try-lock (avoid this from user task
 *                     code; on a busy frame it returns "failed" but you
 *                     can't tell, and subsequent LVGL calls UB)
 *   timeout_ms >  0   wait up to that many ms
 *
 * Returns true if the lock was acquired. Most BSPs declare this as
 * `bool` returning true on success.
 */
bool bsp_display_lock(uint32_t timeout_ms);

/* Release the LVGL mutex. Pair with a successful bsp_display_lock. */
void bsp_display_unlock(void);

#ifdef __cplusplus
}
#endif
