/*
 * harness/screenshot — LVGL screen capture + downsample + base64 emit.
 *
 * Public surface is one console command (`?dump [w=N]`) plus an
 * internal-use registration entry. Most consumers just call
 * `harness_default_register()` (which registers ?dump along with the
 * other default cmds) and never touch this header directly.
 *
 * What ?dump does:
 *   1. Snapshot the active screen as RGB565 into a persistent PSRAM
 *      draw buffer. (~434 KB at 466x466; allocated once, reused.)
 *   2. Snapshot lv_layer_top() as ARGB8888 transiently and composite
 *      over the screen — so the UI chrome (indicator dots, scene name)
 *      is included.
 *   3. Box-filter downsample to target_w x target_w (default 128;
 *      32..256 range).
 *   4. Base64-encode and emit via console_begin_payload("DUMP", ...).
 *
 * The toolkit's `esp-harness screenshot` is the host-side consumer.
 *
 * Threading: the snapshot runs on the LVGL task via lv_async_call (it
 * needs the LVGL mutex which the LVGL task already holds between
 * frame timers). The console caller blocks on a semaphore up to 3 s.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Register the ?dump console command. Called by harness_default_register;
 * standalone projects that want screenshot but not the rest of the
 * default set can call this directly after console_protocol_init(). */
void harness_screenshot_register(void);

#ifdef __cplusplus
}
#endif
