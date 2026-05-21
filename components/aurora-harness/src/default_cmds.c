/*
 * default_cmds.c — opt-in standard console commands. See the header for
 * the rationale of what lives here vs in the consuming app.
 */

#include "harness/default_cmds.h"
#include "harness/console_protocol.h"
#include "harness/scene_framework.h"
#include "harness/screenshot.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "harness/bsp_iface.h"   /* minimal BSP surface — see header */
#include "esp_heap_caps.h"
#include "esp_timer.h"

/* ── ?stat ─────────────────────────────────────────────────────────── */

static volatile uint32_t s_frame_count = 0;
static volatile int64_t  s_frame_window_start_us = 0;
static volatile uint32_t s_frame_window_count = 0;
static volatile float    s_fps_cached = 0.0f;

void harness_record_frame(void)
{
    s_frame_count++;
    s_frame_window_count++;
    int64_t now = esp_timer_get_time();
    if (s_frame_window_start_us == 0) {
        s_frame_window_start_us = now;
        return;
    }
    int64_t elapsed = now - s_frame_window_start_us;
    if (elapsed >= 500000) {  /* recompute fps every 500 ms */
        s_fps_cached = (float)s_frame_window_count * 1000000.0f / (float)elapsed;
        s_frame_window_count = 0;
        s_frame_window_start_us = now;
    }
}

static int cmd_stat(const console_args_t *args)
{
    (void)args;
    size_t heap_free   = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t heap_min    = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    size_t psram_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t int_free    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    int64_t uptime_us  = esp_timer_get_time();
    const scene_t *cur = scene_fw_current();
    int idx            = scene_fw_current_index();

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"fps\":%.1f,\"frame\":%" PRIu32 ","
             "\"heap_free\":%u,\"heap_min\":%u,\"psram_free\":%u,"
             "\"int_free\":%u,\"int_largest\":%u,"
             "\"uptime_ms\":%" PRId64 ","
             "\"scene_idx\":%d,\"scene_id\":\"%s\",\"scene_count\":%d}",
             s_fps_cached, s_frame_count,
             (unsigned)heap_free, (unsigned)heap_min, (unsigned)psram_free,
             (unsigned)int_free, (unsigned)int_largest,
             uptime_us / 1000,
             idx, cur ? cur->id : "-",
             scene_fw_count());
    console_reply_ok("%s", buf);
    return 0;
}

static const console_cmd_t s_cmd_stat = { "?stat", cmd_stat,
    "JSON status: fps, heap, scene" };

/* ── scene ───────────────────────────────────────────────────────── */

static int cmd_scene(const console_args_t *args)
{
    if (args->argc < 2) {
        console_reply_err("usage: scene next|prev|N|<id>|name|action|list");
        return 1;
    }
    const char *sub = args->argv[1];

    /* "name" — read-only, no LVGL mutation. */
    if (strcmp(sub, "name") == 0) {
        const scene_t *c = scene_fw_current();
        console_reply_ok("idx=%d id=%s name=%s",
                         scene_fw_current_index(),
                         c ? c->id : "-",
                         c ? c->display_name : "-");
        return 0;
    }

    /* "list" — JSON manifest of every registered scene + metadata. */
    if (strcmp(sub, "list") == 0) {
        int n = scene_fw_count();
        int cur = scene_fw_current_index();
        console_reply_ok("scene manifest follows");
        console_begin_payload("SCENES", "fmt=json");
        printf("{\"count\":%d,\"current\":%d,\"scenes\":[", n, cur);
        for (int i = 0; i < n; ++i) {
            const scene_t *sc = scene_fw_get(i);
            if (!sc) continue;
            printf("%s{\"idx\":%d,\"id\":\"%s\",\"name\":\"%s\","
                   "\"on_long_press\":%s,"
                   "\"description\":\"%s\","
                   "\"tags\":\"%s\"}",
                   i == 0 ? "" : ",",
                   i, sc->id ? sc->id : "?",
                   sc->display_name ? sc->display_name : "?",
                   sc->on_long_press ? "true" : "false",
                   sc->description ? sc->description : "",
                   sc->tags ? sc->tags : "");
        }
        printf("]}");
        fflush(stdout);
        console_end_payload("SCENES");
        return 0;
    }

    /* Mutating paths — hold LVGL lock. -1 = wait forever; 0 was a
     * non-blocking try that silently failed on a busy frame and wedged
     * the console task in a watchdog loop. See HISTORY.md. */
    bsp_display_lock(-1);

    if (strcmp(sub, "next") == 0) {
        scene_fw_next();
    } else if (strcmp(sub, "prev") == 0) {
        scene_fw_prev();
    } else if (strcmp(sub, "action") == 0) {
        const scene_t *c = scene_fw_current();
        if (!c || !c->on_long_press) {
            bsp_display_unlock();
            console_reply_err("scene: current scene has no action");
            return 1;
        }
        lv_async_call((lv_async_cb_t)c->on_long_press, (void *)c);
        bsp_display_unlock();
        console_reply_ok("action  idx=%d  id=%s", scene_fw_current_index(), c->id);
        return 0;
    } else {
        char *end = NULL;
        long idx = strtol(sub, &end, 10);
        if (end == sub) {
            /* Not a number — try id lookup ("halo", "survey", ...). */
            int found = scene_fw_find_by_id(sub);
            if (found < 0) {
                bsp_display_unlock();
                console_reply_err("scene: unknown subcommand '%s' (try a number, an id, or 'list')", sub);
                return 1;
            }
            scene_fw_show(found);
        } else {
            scene_fw_show((int)idx);
        }
    }
    /* scene_fw_show fires the change_listener if one is set — the
     * consuming app's UI chrome (if any) updates itself there. */
    const scene_t *c = scene_fw_current();
    bsp_display_unlock();
    console_reply_ok("idx=%d id=%s", scene_fw_current_index(), c ? c->id : "-");
    return 0;
}

static const console_cmd_t s_cmd_scene = { "scene", cmd_scene,
    "scene next|prev|N|<id>|name|action|list — N=index, <id>=halo|survey|... (list returns JSON manifest)" };

/* ── tap / swipe — synthetic touch via LVGL indev primitives ─────── */

typedef struct { int x; int y; } harness_point_t;

static void tap_at_async(void *user)
{
    harness_point_t *pt = (harness_point_t *)user;
    lv_obj_t *scr = lv_screen_active();
    if (!scr) { free(pt); return; }
    lv_point_t p = { (lv_coord_t)pt->x, (lv_coord_t)pt->y };
    lv_obj_t *target = lv_indev_search_obj(scr, &p);
    if (target == NULL) target = scr;
    lv_obj_send_event(target, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(target, LV_EVENT_RELEASED, NULL);
    lv_obj_send_event(target, LV_EVENT_CLICKED, NULL);
    console_send_evt("tap_hit x=%d y=%d obj=%p", pt->x, pt->y, target);
    free(pt);
}

static int cmd_tap(const console_args_t *args)
{
    harness_point_t *pt = (harness_point_t *)malloc(sizeof(*pt));
    if (!pt) { console_reply_err("oom"); return 1; }
    if (args->argc >= 3) {
        pt->x = atoi(args->argv[1]);
        pt->y = atoi(args->argv[2]);
    } else {
        lv_obj_t *scr = lv_screen_active();
        pt->x = lv_obj_get_width(scr) / 2;
        pt->y = lv_obj_get_height(scr) / 2;
    }
    lv_async_call(tap_at_async, pt);
    console_reply_ok("tap %d %d dispatched", pt->x, pt->y);
    return 0;
}

typedef struct { int x1, y1, x2, y2; int steps; } harness_swipe_t;

static void swipe_async(void *user)
{
    harness_swipe_t *sw = (harness_swipe_t *)user;
    lv_obj_t *scr = lv_screen_active();
    lv_point_t p0 = { (lv_coord_t)sw->x1, (lv_coord_t)sw->y1 };
    lv_obj_t *target = lv_indev_search_obj(scr, &p0);
    if (target == NULL) target = scr;

    lv_obj_send_event(target, LV_EVENT_PRESSED, NULL);
    for (int i = 1; i <= sw->steps; ++i) {
        lv_obj_send_event(target, LV_EVENT_PRESSING, NULL);
    }
    lv_obj_send_event(target, LV_EVENT_RELEASED, NULL);

    /* Synthesise a directional GESTURE event so swipe-handlers fire. */
    lv_dir_t dir = LV_DIR_NONE;
    int dx = sw->x2 - sw->x1, dy = sw->y2 - sw->y1;
    if (abs(dx) > abs(dy)) dir = (dx > 0) ? LV_DIR_RIGHT : LV_DIR_LEFT;
    else                   dir = (dy > 0) ? LV_DIR_BOTTOM : LV_DIR_TOP;
    lv_obj_send_event(target, LV_EVENT_GESTURE, &dir);

    console_send_evt("swipe_done from=%d,%d to=%d,%d dir=%d",
                     sw->x1, sw->y1, sw->x2, sw->y2, (int)dir);
    free(sw);
}

static int cmd_swipe(const console_args_t *args)
{
    if (args->argc < 5) {
        console_reply_err("usage: swipe X1 Y1 X2 Y2 [DUR_MS]");
        return 1;
    }
    harness_swipe_t *sw = (harness_swipe_t *)malloc(sizeof(*sw));
    if (!sw) { console_reply_err("oom"); return 1; }
    sw->x1 = atoi(args->argv[1]);
    sw->y1 = atoi(args->argv[2]);
    sw->x2 = atoi(args->argv[3]);
    sw->y2 = atoi(args->argv[4]);
    int dur_ms = (args->argc >= 6) ? atoi(args->argv[5]) : 200;
    sw->steps = (dur_ms / 16) + 2;
    lv_async_call(swipe_async, sw);
    console_reply_ok("swipe (%d,%d)->(%d,%d) %dms dispatched",
                     sw->x1, sw->y1, sw->x2, sw->y2, dur_ms);
    return 0;
}

static const console_cmd_t s_cmd_tap   = { "tap",   cmd_tap,
    "tap [X Y]; no args = screen centre" };
static const console_cmd_t s_cmd_swipe = { "swipe", cmd_swipe,
    "swipe X1 Y1 X2 Y2 [DUR_MS]" };

/* ── registration entry point ──────────────────────────────────────── */

void harness_default_register(void)
{
    console_protocol_register(&s_cmd_stat);
    console_protocol_register(&s_cmd_scene);
    console_protocol_register(&s_cmd_tap);
    console_protocol_register(&s_cmd_swipe);
    harness_screenshot_register();  /* ?dump */
}
