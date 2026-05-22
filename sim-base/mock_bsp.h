/*
 * mock_bsp.h — host stubs for ESP-IDF BSP APIs.
 *
 * The target firmware uses `#include "bsp/esp-bsp.h"` for display init,
 * brightness control, and the LVGL lock. On host, none of those mean
 * anything; we just shim the same prototypes so existing scene code
 * compiles unchanged.
 *
 * NOTE: include this in your host build's include path BEFORE LVGL,
 * but only in main.c (not in scene .c files — those still see
 * `bsp/esp-bsp.h` via a wrapper header below).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* No-op locks — SDL backend uses LVGL's own thread safety. Returns
 * `true` to match the signature aurora-harness's bsp_iface.h declares
 * (the void variant here was a long-standing UB-leaning mismatch
 * that worked only because callers discarded the return). */
static inline bool bsp_display_lock(uint32_t timeout_ms)   { (void)timeout_ms; return true; }
static inline void bsp_display_unlock(void)                {}

/* Brightness is a no-op; we log it instead so scene_glow visibly works. */
void bsp_display_brightness_set(int pct);

/* Display init runs as part of main(), this stub satisfies the symbol
 * if any scene re-asserts it. */
lv_display_t *bsp_display_start(void);

#ifdef __cplusplus
}
#endif
