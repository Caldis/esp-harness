/*
 * harness/default_cmds — opt-in registration of universally-useful console
 * commands. Call harness_default_register() once, after
 * console_protocol_init(), to gain the entire standard set in one line.
 *
 * Commands registered (v1.4):
 *
 *   ?stat     JSON: fps + heap (free/min/internal/psram) + scene
 *             (idx/id/count) + uptime_ms. Tick harness_record_frame()
 *             each frame for the fps field to update; the rest works
 *             without it.
 *   scene     framework navigation: next | prev | <N> | <id> | name |
 *             action | list. `scene list` emits a SCENES_BEGIN/END
 *             JSON manifest of every registered scene with description
 *             + tags. Application UI chrome listens via
 *             scene_fw_set_change_listener (see scene_framework.h).
 *   tap       synthetic LVGL touch (defaults to screen centre) — uses
 *             lv_async_call so safe from any task.
 *   swipe     "swipe X1 Y1 X2 Y2 [DUR_MS]" — synthetic gesture with
 *             intermediate PRESSING events so LVGL swipe-handlers fire.
 *   ?dump     LVGL screenshot: snapshots the active screen + composites
 *             the top layer, box-filter downsamples to N×N (32..256,
 *             default 128), and emits as base64 RGB565 in a DUMP
 *             payload frame. PSRAM-backed; first call lazy-allocates
 *             434 KB and reuses it forever. See harness/screenshot.h.
 *
 * Keep this set small and ruthlessly generic — peripheral-coupled commands
 * (?audio / ?sd / wifi / ble / ?power / ?sys / ?ota) belong in the
 * consuming app, not here.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Register the default command set. Must be called after
 * console_protocol_init(). Safe to call from app_main on the
 * non-LVGL task. */
void harness_default_register(void);

/* Bump the per-frame counter feeding ?stat's fps field. Call this once
 * per LVGL frame, typically from a lv_timer at ~30 Hz:
 *
 *     static void frame_cb(lv_timer_t *t) {
 *         (void)t;
 *         harness_record_frame();
 *     }
 *     lv_timer_create(frame_cb, 33, NULL);
 *
 * fps recomputes every 500 ms over a sliding window. Without this call
 * ?stat's fps field reads 0 — everything else (heap / scene_idx / uptime)
 * still works.
 */
void harness_record_frame(void);

#ifdef __cplusplus
}
#endif
