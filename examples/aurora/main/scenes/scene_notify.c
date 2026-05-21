/*
 * Scene XIX · Notify — demo + reference for harness_toast().
 *
 * Five toast variants cycle through on BOOT presses. USER triggers a
 * long-hold toast (3 s) to show that durations work. Counter in the
 * corner tracks how many toasts fired so far.
 *
 * Purpose: this scene exists primarily as the *reference implementation*
 * for using the toast primitive — copy-paste from here when building
 * your own toast-using scene.
 */

#include "harness/scene_framework.h"
#include "harness/toast.h"
#include "lvgl.h"
#include "peripherals/keys.h"
#include <stdio.h>

#define ACCENT  0xF0E0A8       /* warm sand */

static const char *kVariants[] = {
    "first toast",
    "still works",
    "BOOT cycles me",
    "USER for long hold",
    "press again",
};
#define VARIANT_COUNT  (int)(sizeof(kVariants) / sizeof(kVariants[0]))

typedef struct {
    lv_obj_t *roman;
    lv_obj_t *hint_top;
    lv_obj_t *hint_bottom;
    lv_obj_t *counter;
    int       last_boot_count;
    int       last_user_count;
    int       fired_count;
    int       variant_idx;
} notify_state_t;

static void update_counter(notify_state_t *st)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "fired %d", st->fired_count);
    lv_label_set_text(st->counter, buf);
}

static void notify_frame(scene_t *s, uint32_t t_ms)
{
    (void)t_ms;
    notify_state_t *st = (notify_state_t *)s->user_data;
    if (!st) return;
    keys_state_t k;
    keys_get(&k);
    if ((int)k.boot_count != st->last_boot_count) {
        st->last_boot_count = (int)k.boot_count;
        if (k.boot_pressed) {
            harness_toast(kVariants[st->variant_idx], 1500);
            st->variant_idx = (st->variant_idx + 1) % VARIANT_COUNT;
            st->fired_count++;
            update_counter(st);
        }
    }
    if ((int)k.user_count != st->last_user_count) {
        st->last_user_count = (int)k.user_count;
        if (k.user_pressed) {
            harness_toast("long hold (3 s)", 3000);
            st->fired_count++;
            update_counter(st);
        }
    }
}

static void notify_on_show(scene_t *s)
{
    notify_state_t *st = (notify_state_t *)s->user_data;
    if (!st) return;
    keys_state_t k;
    keys_get(&k);
    st->last_boot_count = (int)k.boot_count;
    st->last_user_count = (int)k.user_count;
}

static void notify_on_hide(scene_t *s)
{
    (void)s;
    /* Be polite: clear any in-flight toast when leaving the scene so it
     * doesn't bleed over onto the next one. */
    harness_toast_dismiss();
}

static void notify_init(scene_t *s, lv_obj_t *parent)
{
    notify_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;
    st->variant_idx = 0;

    /* Roman XIX */
    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(st->roman, 4, 0);
    lv_obj_set_style_text_color(st->roman, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_50, 0);
    lv_label_set_text(st->roman, "XIX");
    lv_obj_align(st->roman, LV_ALIGN_TOP_MID, 0, 100);

    /* Title / hint stack */
    lv_obj_t *title = lv_label_create(parent);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_opa(title, LV_OPA_90, 0);
    lv_label_set_text(title, "Notify");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);

    st->hint_top = lv_label_create(parent);
    lv_obj_set_style_text_font(st->hint_top, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->hint_top, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->hint_top, LV_OPA_70, 0);
    lv_label_set_text(st->hint_top, "BOOT  cycle toast");
    lv_obj_align(st->hint_top, LV_ALIGN_CENTER, 0, 0);

    st->hint_bottom = lv_label_create(parent);
    lv_obj_set_style_text_font(st->hint_bottom, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->hint_bottom, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->hint_bottom, LV_OPA_70, 0);
    lv_label_set_text(st->hint_bottom, "USER  long hold (3 s)");
    lv_obj_align(st->hint_bottom, LV_ALIGN_CENTER, 0, 24);

    st->counter = lv_label_create(parent);
    lv_obj_set_style_text_font(st->counter, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(st->counter, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->counter, LV_OPA_50, 0);
    lv_label_set_text(st->counter, "fired 0");
    lv_obj_align(st->counter, LV_ALIGN_BOTTOM_MID, 0, -90);
}

scene_t scene_notify = {
    .id            = "notify",
    .display_name  = "XIX. Notify",
    .description   = "Reference demo for harness_toast(); BOOT cycles 5 variants, USER long-holds",
    .tags          = "demo,toast,reference",
    .accent        = LV_COLOR_MAKE(0xF0, 0xE0, 0xA8),
    .init          = notify_init,
    .on_show       = notify_on_show,
    .on_hide       = notify_on_hide,
    .frame         = notify_frame,
};
