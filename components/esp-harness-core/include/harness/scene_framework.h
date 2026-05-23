/*
 * scene_framework — Aurora's stage manager.
 *
 * A "scene" is a self-contained generative visual that owns a parent LVGL
 * container. It implements four lifecycle hooks:
 *
 *   init(parent)      — build LVGL objects, allocate state. Called once.
 *   on_show()         — about to become visible. Reset transient state.
 *   on_hide()         — about to be hidden. Pause timers / animations.
 *   frame(t_ms)       — optional per-frame tick from a shared timer.
 *   on_tilt(ax,ay,az) — optional IMU update (gravity in g units).
 *
 * The framework keeps only ONE scene visible at a time. Switching is a
 * brief crossfade to avoid jarring transitions.
 */

#pragma once

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct scene scene_t;

struct scene {
    const char *id;            /* "particles", lowercase, stable */
    const char *display_name;  /* "I. Particles" — shown in UI */
    lv_color_t  accent;        /* per-scene accent colour */

    /* Optional metadata — both default NULL. Surfaced by `scene list`
     * JSON so an AI agent reading the manifest knows what each scene
     * exercises without grepping source. */
    const char *description;   /* one-line natural-language summary */
    const char *tags;          /* comma-separated, e.g. "imu,accel" or
                                * "wifi,ble,radio". Used for category
                                * filtering by toolkit aggregators. */

    void (*init)(scene_t *s, lv_obj_t *parent);
    void (*on_show)(scene_t *s);
    void (*on_hide)(scene_t *s);
    void (*frame)(scene_t *s, uint32_t t_ms);
    void (*on_tilt)(scene_t *s, float ax, float ay, float az);
    /* Scene-specific action triggered by a long press (≥400ms hold).
     * Short tap continues to navigate to the next scene. Used by
     * peripheral scenes to invoke their thing (play tone, refresh SD,
     * trigger BLE/WiFi scan, etc). */
    void (*on_long_press)(scene_t *s);
    /* Fires on every finger-lift, regardless of short/long. Scenes
     * doing "press-and-hold" actions (e.g. Listen recording while
     * held) use this to stop the action on release. Non-interactive
     * scenes leave it NULL and it's a no-op. */
    void (*on_release)(scene_t *s);

    /* Private state — scene implementations stash their own pointers/data
     * here so they don't need globals. */
    void *user_data;
    lv_obj_t *container;   /* set by framework; the parent LVGL object */
    bool initialised;
};

/* Mount the framework under the given root and start the frame timer. */
void scene_fw_init(lv_obj_t *root);

/* Register a scene (pointer must outlive the program). Order = display order. */
void scene_fw_register(scene_t *scene);

/* Switch to scene by index (mod count). Triggers a brief crossfade. */
void scene_fw_show(int idx);

/* Convenience: next / previous with wrap. */
void scene_fw_next(void);
void scene_fw_prev(void);

/* Push IMU sample to the currently visible scene (if it implements on_tilt). */
void scene_fw_push_tilt(float ax, float ay, float az);

/* Introspection */
int scene_fw_count(void);
int scene_fw_current_index(void);
const scene_t *scene_fw_current(void);
const scene_t *scene_fw_get(int idx);

/* Lookup by id ("halo", "survey", ...). Linear scan. Returns -1 if not
 * found. Useful for cmd_scene's id-based switching. */
int scene_fw_find_by_id(const char *id);

/* Change listener — fires AFTER scene_fw_show completes, on the LVGL
 * task. Pass NULL to clear. Typical use: a consuming app's UI chrome
 * (indicator dot, status label) listens to update itself, keeping the
 * scene framework itself unaware of any chrome layer.
 *
 * Set ONCE at startup. Replacing the listener mid-flight is safe but
 * pointless — pick the one consumer that owns chrome and stick with it. */
typedef void (*scene_change_listener_t)(int idx, const scene_t *current);
void scene_fw_set_change_listener(scene_change_listener_t cb);

#ifdef __cplusplus
}
#endif
