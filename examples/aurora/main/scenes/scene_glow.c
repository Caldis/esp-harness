/*
 * Scene XV · Glow — AMOLED brightness control.
 *
 * BOOT (-) / USER (+) cycle through brightness in 10 % steps.
 * Persisted to NVS through settings_set_brightness so it survives a
 * reboot.
 *
 * The big "75 %" number is the obvious feedback, but the entire screen
 * also dims/brightens in real time, so even without looking at the
 * label you can tell something is happening.
 */

#include "harness/scene_framework.h"
#include "lvgl.h"
#include "peripherals/keys.h"
#include "peripherals/settings.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include <stdio.h>

#define ACCENT       0xFFE08C     /* warm gold */
#define STEP         10           /* 10 % per press */
#define BAR_W        220
#define BAR_H        14

typedef struct {
    lv_obj_t *roman;
    lv_obj_t *big;            /* "75 %" */
    lv_obj_t *track;
    lv_obj_t *bar;
    lv_obj_t *hint;
    lv_timer_t *timer;
    uint32_t last_boot_count;
    uint32_t last_user_count;
    int      shown_pct;
} glow_state_t;

static void apply_brightness(glow_state_t *st, int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct == st->shown_pct) return;
    st->shown_pct = pct;
    /* BSP wraps the LCD-controller-specific dimming command. */
    bsp_display_brightness_set(pct);
    settings_set_brightness(pct);
    char buf[12];
    snprintf(buf, sizeof(buf), "%d %%", pct);
    lv_label_set_text(st->big, buf);
    lv_obj_set_width(st->bar, (BAR_W * pct) / 100);
}

static void glow_tick(lv_timer_t *t)
{
    glow_state_t *st = (glow_state_t *)lv_timer_get_user_data(t);
    if (!st) return;
    keys_state_t k;
    keys_get(&k);
    if (k.boot_count != st->last_boot_count) {
        st->last_boot_count = k.boot_count;
        apply_brightness(st, st->shown_pct - STEP);
    }
    if (k.user_count != st->last_user_count) {
        st->last_user_count = k.user_count;
        apply_brightness(st, st->shown_pct + STEP);
    }
}

static void glow_init(scene_t *s, lv_obj_t *parent)
{
    glow_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;
    settings_t cur;
    settings_get(&cur);
    st->shown_pct = -1;   /* force first apply_brightness to write */

    /* Roman XV */
    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(st->roman, 6, 0);
    lv_obj_set_style_text_color(st->roman, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_60, 0);
    lv_label_set_text(st->roman, "XV");
    lv_obj_align(st->roman, LV_ALIGN_TOP_MID, 0, 100);

    /* Big percent */
    st->big = lv_label_create(parent);
    lv_obj_set_style_text_font(st->big, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(st->big, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->big, LV_OPA_90, 0);
    lv_label_set_text(st->big, "-");
    lv_obj_align(st->big, LV_ALIGN_CENTER, 0, -20);

    /* Track + filled bar */
    st->track = lv_obj_create(parent);
    lv_obj_remove_style_all(st->track);
    lv_obj_set_size(st->track, BAR_W, BAR_H);
    lv_obj_align(st->track, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_style_radius(st->track, BAR_H / 2, 0);
    lv_obj_set_style_bg_opa(st->track, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(st->track, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_border_width(st->track, 1, 0);
    lv_obj_set_style_border_opa(st->track, LV_OPA_30, 0);
    lv_obj_clear_flag(st->track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(st->track, LV_OBJ_FLAG_SCROLLABLE);

    st->bar = lv_obj_create(parent);
    lv_obj_remove_style_all(st->bar);
    lv_obj_set_size(st->bar, 0, BAR_H);
    lv_obj_set_style_radius(st->bar, BAR_H / 2, 0);
    lv_obj_set_style_bg_color(st->bar, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_bg_opa(st->bar, LV_OPA_90, 0);
    lv_obj_set_style_border_width(st->bar, 0, 0);
    lv_obj_align_to(st->bar, st->track, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_clear_flag(st->bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(st->bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Hint */
    st->hint = lv_label_create(parent);
    lv_obj_set_style_text_font(st->hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->hint, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->hint, LV_OPA_50, 0);
    lv_label_set_text(st->hint, "boot / user adjusts brightness");
    lv_obj_align(st->hint, LV_ALIGN_CENTER, 0, 65);

    /* Sync key counters. */
    keys_state_t k;
    keys_get(&k);
    st->last_boot_count = k.boot_count;
    st->last_user_count = k.user_count;

    /* Apply saved brightness on first paint. */
    apply_brightness(st, cur.brightness_pct);

    st->timer = lv_timer_create(glow_tick, 50, st);
    lv_timer_pause(st->timer);
}

static void glow_on_show(scene_t *s)
{
    glow_state_t *st = (glow_state_t *)s->user_data;
    if (!st) return;
    keys_state_t k;
    keys_get(&k);
    st->last_boot_count = k.boot_count;
    st->last_user_count = k.user_count;
    if (st->timer) lv_timer_resume(st->timer);
}
static void glow_on_hide(scene_t *s)
{
    glow_state_t *st = (glow_state_t *)s->user_data;
    if (st && st->timer) lv_timer_pause(st->timer);
}

scene_t scene_glow = {
    .id           = "glow",
    .display_name = "XV. Glow",
    .accent       = LV_COLOR_MAKE(0xFF, 0xE0, 0x8C),
    .description  = "AMOLED brightness control: BOOT (-) / USER (+) in 10% steps, persisted to NVS",
    .tags         = "display,settings,interactive,nvs",
    .init         = glow_init,
    .on_show      = glow_on_show,
    .on_hide      = glow_on_hide,
};
