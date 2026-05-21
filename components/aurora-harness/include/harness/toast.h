/*
 * harness/toast — fire-and-forget overlay notification.
 *
 * Pops a translucent label onto `lv_layer_top()` (above any scene),
 * holds it for `duration_ms`, then quietly removes itself. Multiple
 * calls in flight: the latest one replaces the prior one immediately.
 *
 * Thread-safe: dispatches LVGL widget mutations via `lv_async_call`,
 * so this is callable from any FreeRTOS task — including a console
 * command handler, a sensor task, or an IRQ-deferred handler.
 *
 *   harness_toast("captured", 1500);          // 1.5 s default-style
 *   harness_toast("battery low", 3000);       // longer hold
 *
 * Memory: each call mallocs a small request struct (~120 bytes) freed
 * inside the async dispatch. No leak even if the toast is replaced.
 *
 * Style is intentionally fixed (white text on translucent black,
 * lower-middle alignment, 14 pt Montserrat). If you need per-toast
 * styling, this primitive is the wrong fit — write your own scene
 * widget.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Show a toast. text is copied. duration_ms = 0 means default (1500 ms). */
void harness_toast(const char *text, uint32_t duration_ms);

/* Immediately dismiss any active toast (useful for cleanup before scene
 * transitions if the previous scene queued a long-duration toast). */
void harness_toast_dismiss(void);

#ifdef __cplusplus
}
#endif
