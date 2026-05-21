/*
 * scene_framework implementation.
 *
 * Each registered scene gets its own full-screen `lv_obj_t` container.
 * Containers are created at register-time; the framework toggles
 * `LV_OBJ_FLAG_HIDDEN` to show one at a time. A brief fade is done via
 * opacity transitions handled by LVGL's built-in animation engine.
 *
 * A shared timer fires at ~60 Hz and dispatches frame() to the visible
 * scene. Scenes that don't need a tick just leave frame=NULL.
 */

#include "harness/scene_framework.h"

#include <string.h>

/* scene_framework runs entirely on the LVGL task — no bsp_display_lock
 * calls. Removed stale include in v1.4. */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "scene_fw"
/* MAX_SCENES bounds the static s_scenes[] array. Bumped to 24 in v1.1
 * (16 -> 24) after Scene XVII Survey + XVIII Sniff hit the old cap
 * silently — scene_fw_register early-returns on overflow without an
 * error log, so the AI manifest showed 16 while aurora_main called 18
 * register()s. If you add another scene and `?stat scene_count` doesn't
 * match `aurora_main.c::kSceneCount`, bump this. */
#define MAX_SCENES 24
/* 33 ms ≈ 30 Hz. We tried 60 Hz first; the CO5300 SPI bus couldn't keep up
 * with all the per-frame partial-redraws coming from widget-heavy scenes
 * (DMA TX underflow). 30 Hz is buttery on AMOLED anyway and leaves plenty
 * of headroom for things like ?dump to acquire the LVGL lock. */
#define FRAME_PERIOD_MS 33
#define FADE_MS 280

static lv_obj_t *s_root = NULL;
static scene_t  *s_scenes[MAX_SCENES];
static int       s_count = 0;
static int       s_current = -1;
static lv_timer_t *s_frame_timer = NULL;
static scene_change_listener_t s_change_listener = NULL;

static void frame_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (s_current < 0) return;
    scene_t *s = s_scenes[s_current];
    if (s && s->frame) {
        s->frame(s, (uint32_t)lv_tick_get());
    }
}

static void hide_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
    if (v == 0) {
        lv_obj_add_flag((lv_obj_t *)obj, LV_OBJ_FLAG_HIDDEN);
    }
}
static void show_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

void scene_fw_init(lv_obj_t *root)
{
    s_root = root;
    s_count = 0;
    s_current = -1;
    /* Frame timer scheduled — runs in LVGL task context, no locking needed. */
    s_frame_timer = lv_timer_create(frame_tick_cb, FRAME_PERIOD_MS, NULL);
    ESP_LOGI(TAG, "framework mounted, frame tick %d ms", FRAME_PERIOD_MS);
}

void scene_fw_register(scene_t *scene)
{
    if (!scene) return;
    if (s_count >= MAX_SCENES) {
        ESP_LOGE(TAG, "scene_fw_register dropped '%s' — MAX_SCENES=%d full. "
                       "Bump it in scene_framework.c.",
                 scene->id ? scene->id : "?", MAX_SCENES);
        return;
    }

    /* Each scene gets a full-screen container under root. */
    lv_obj_t *c = lv_obj_create(s_root);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_PCT(100), LV_PCT(100));
    lv_obj_center(c);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    /* The container must not absorb touch events — the screen-level tap
     * handler in aurora_main needs them to advance scenes. lv_obj_create
     * sets CLICKABLE by default; clear it so events fall through to the
     * parent (the active screen). */
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);  /* invisible until shown */
    lv_obj_set_style_bg_color(c, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(c, LV_OPA_TRANSP, 0);

    scene->container = c;
    s_scenes[s_count++] = scene;

    if (scene->init) {
        scene->init(scene, c);
    }
    scene->initialised = true;

    ESP_LOGI(TAG, "registered scene %d: %s", s_count - 1, scene->id);

    /* Auto-show the first registered scene. */
    if (s_count == 1) {
        scene_fw_show(0);
    }
}

void scene_fw_show(int idx)
{
    if (s_count == 0) return;
    /* Wrap around */
    idx = ((idx % s_count) + s_count) % s_count;
    if (idx == s_current) return;

    int prev = s_current;
    s_current = idx;

    scene_t *next = s_scenes[idx];

    /* Bring next to top of stack & make visible (transparent). */
    lv_obj_move_foreground(next->container);
    lv_obj_clear_flag(next->container, LV_OBJ_FLAG_HIDDEN);
    if (next->on_show) next->on_show(next);

    /* Fade-in next. */
    lv_anim_t a_in;
    lv_anim_init(&a_in);
    lv_anim_set_var(&a_in, next->container);
    lv_anim_set_values(&a_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a_in, FADE_MS);
    lv_anim_set_path_cb(&a_in, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a_in, show_anim_cb);
    lv_anim_start(&a_in);

    if (prev >= 0) {
        scene_t *p = s_scenes[prev];
        /* Fade-out prev. */
        lv_anim_t a_out;
        lv_anim_init(&a_out);
        lv_anim_set_var(&a_out, p->container);
        lv_anim_set_values(&a_out, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_duration(&a_out, FADE_MS);
        lv_anim_set_path_cb(&a_out, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a_out, hide_anim_cb);
        lv_anim_start(&a_out);

        if (p->on_hide) p->on_hide(p);
    }

    ESP_LOGI(TAG, "show scene %d (%s)", idx, next->id);

    /* Notify the consumer (typically the UI chrome layer) AFTER the
     * framework's own state is settled. Listener runs on LVGL task so
     * it can mutate widgets directly. */
    if (s_change_listener) {
        s_change_listener(idx, next);
    }
}

void scene_fw_next(void) { scene_fw_show(s_current + 1); }
void scene_fw_prev(void) { scene_fw_show(s_current - 1); }

void scene_fw_push_tilt(float ax, float ay, float az)
{
    if (s_current < 0) return;
    scene_t *s = s_scenes[s_current];
    if (s && s->on_tilt) s->on_tilt(s, ax, ay, az);
}

int scene_fw_count(void) { return s_count; }
int scene_fw_current_index(void) { return s_current; }
const scene_t *scene_fw_current(void)
{
    return (s_current >= 0) ? s_scenes[s_current] : NULL;
}
const scene_t *scene_fw_get(int idx)
{
    if (idx < 0 || idx >= s_count) return NULL;
    return s_scenes[idx];
}

int scene_fw_find_by_id(const char *id)
{
    if (!id) return -1;
    for (int i = 0; i < s_count; ++i) {
        const scene_t *sc = s_scenes[i];
        if (sc && sc->id && strcmp(sc->id, id) == 0) return i;
    }
    return -1;
}

void scene_fw_set_change_listener(scene_change_listener_t cb)
{
    s_change_listener = cb;
}
