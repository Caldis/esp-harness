/*
 * harness/progress — overlay progress bar (long-running task indicator).
 *
 * Sister primitive to harness_toast(). Where toast is fire-and-forget
 * with a time-based auto-expire, progress has an explicit lifecycle:
 *
 *   harness_progress_show("Downloading", 0);
 *   for (int i = 0; i <= 100; ++i) {
 *       do_work_step();
 *       harness_progress_show("Downloading", i);
 *   }
 *   harness_progress_dismiss();
 *
 * The first call creates the overlay (label + bar on lv_layer_top()).
 * Subsequent calls update the existing widget in place — no churn.
 * dismiss removes it.
 *
 * Use cases: OTA download progress, sector-by-sector erase, long-running
 * scan, file upload. Anything where the user wants to see "X% done"
 * during a multi-second blocking operation.
 *
 * Thread-safe via lv_async_call, same pattern as toast — callable from
 * any FreeRTOS task.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Show / update the progress overlay.
 *   text:    label above the bar (NULL or "" hides the label)
 *   percent: 0..100; values out of range clamp
 * First call creates the overlay. Subsequent calls update in place.
 */
void harness_progress_show(const char *text, int percent);

/* Remove the overlay. Idempotent — safe to call when no progress is up. */
void harness_progress_dismiss(void);

#ifdef __cplusplus
}
#endif
