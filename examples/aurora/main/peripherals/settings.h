/*
 * settings.h — persistent app config in NVS.
 *
 * Stores user-tweakable knobs that should survive a reboot:
 *   - speaker volume (0..100)
 *   - last-active scene index
 *   - AMOLED brightness (0..100)
 *
 * Writes are throttled (one NVS commit per 5 s of pending changes) so
 * fast knob spinning doesn't burn flash erase cycles. Reads are cached
 * after `settings_init` so the GUI paths stay lock-free.
 *
 * Namespace: "aurora". Keys are <16 ASCII chars (NVS limit).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int volume_pct;
    int brightness_pct;
    int last_scene_idx;
} settings_t;

bool settings_init(void);

/* Cached snapshot. Lock-free read. */
void settings_get(settings_t *out);

/* Mark a field dirty and schedule a commit. The actual flash write
 * happens in the background after ~5 s of quiet (or on next reboot's
 * deinit). */
void settings_set_volume(int pct);
void settings_set_brightness(int pct);
void settings_set_last_scene(int idx);

/* Force a sync commit now (e.g. before deep sleep). Cheap if nothing
 * is dirty. */
void settings_flush(void);

#ifdef __cplusplus
}
#endif
