/*
 * Scene XX · Track — reference demo for harness_progress().
 *
 * BOOT starts a simulated 5-second download. The progress overlay
 * counts 0..100 over 100 ticks of 50 ms each. USER dismisses early.
 *
 * Like Scene XIX (Notify) is for harness_toast(), this scene exists
 * primarily so anyone porting/extending knows what real progress code
 * looks like — copy-paste from here.
 */

#include "harness/scene_framework.h"
#include "harness/progress.h"
#include "lvgl.h"
#include "peripherals/keys.h"
#include <stdio.h>

#define ACCENT  0x7CC4A2   /* mint */

typedef struct {
    lv_obj_t *roman;
    lv_obj_t *title;
    lv_obj_t *hint;
    lv_obj_t *state;
    lv_timer_t *tick_timer;
    int       progress;       /* 0..100, -1 = idle */
    int       last_boot_count;
    int       last_user_count;
} track_state_t;

static void tick_cb(lv_timer_t *t)
{
    track_state_t *st = (track_state_t *)lv_timer_get_user_data(t);
    if (!st) return;
    st->progress++;
    if (st->progress > 100) {
        harness_progress_dismiss();
        lv_label_set_text(st->state, "done");
        lv_timer_delete(t);
        st->tick_timer = NULL;
        st->progress = -1;
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "Downloading %d%%", st->progress);
    harness_progress_show(buf, st->progress);
}

static void start_progress(track_state_t *st)
{
    if (st->tick_timer) return;
    st->progress = 0;
    harness_progress_show("Downloading 0%", 0);
    lv_label_set_text(st->state, "running");
    st->tick_timer = lv_timer_create(tick_cb, 50, NULL);
    lv_timer_set_user_data(st->tick_timer, st);
}

static void cancel_progress(track_state_t *st)
{
    if (!st->tick_timer) return;
    lv_timer_delete(st->tick_timer);
    st->tick_timer = NULL;
    st->progress = -1;
    harness_progress_dismiss();
    lv_label_set_text(st->state, "cancelled");
}

static void track_frame(scene_t *s, uint32_t t_ms)
{
    (void)t_ms;
    track_state_t *st = (track_state_t *)s->user_data;
    if (!st) return;
    keys_state_t k;
    keys_get(&k);
    if ((int)k.boot_count != st->last_boot_count) {
        st->last_boot_count = (int)k.boot_count;
        if (k.boot_pressed) start_progress(st);
    }
    if ((int)k.user_count != st->last_user_count) {
        st->last_user_count = (int)k.user_count;
        if (k.user_pressed) cancel_progress(st);
    }
}

static void track_on_show(scene_t *s)
{
    track_state_t *st = (track_state_t *)s->user_data;
    if (!st) return;
    keys_state_t k;
    keys_get(&k);
    st->last_boot_count = (int)k.boot_count;
    st->last_user_count = (int)k.user_count;
}

static void track_on_hide(scene_t *s)
{
    track_state_t *st = (track_state_t *)s->user_data;
    if (!st) return;
    if (st->tick_timer) {
        lv_timer_delete(st->tick_timer);
        st->tick_timer = NULL;
    }
    harness_progress_dismiss();
}

static void track_init(scene_t *s, lv_obj_t *parent)
{
    track_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;
    st->progress = -1;

    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(st->roman, 4, 0);
    lv_obj_set_style_text_color(st->roman, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_50, 0);
    lv_label_set_text(st->roman, "XX");
    lv_obj_align(st->roman, LV_ALIGN_TOP_MID, 0, 100);

    st->title = lv_label_create(parent);
    lv_obj_set_style_text_font(st->title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(st->title, lv_color_white(), 0);
    lv_label_set_text(st->title, "Track");
    lv_obj_align(st->title, LV_ALIGN_CENTER, 0, -50);

    st->hint = lv_label_create(parent);
    lv_obj_set_style_text_font(st->hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->hint, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->hint, LV_OPA_70, 0);
    lv_label_set_text(st->hint, "BOOT start (5 s)   USER cancel");
    lv_obj_align(st->hint, LV_ALIGN_CENTER, 0, -10);

    st->state = lv_label_create(parent);
    lv_obj_set_style_text_font(st->state, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->state, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->state, LV_OPA_50, 0);
    lv_label_set_text(st->state, "idle");
    lv_obj_align(st->state, LV_ALIGN_CENTER, 0, 20);
}

scene_t scene_track = {
    .id            = "track",
    .display_name  = "XX. Track",
    .accent        = LV_COLOR_MAKE(0x7C, 0xC4, 0xA2),
    .description   = "Reference demo for harness_progress(); BOOT runs 5s download, USER cancels",
    .tags          = "demo,progress,reference",
    .init          = track_init,
    .on_show       = track_on_show,
    .on_hide       = track_on_hide,
    .frame         = track_frame,
};
