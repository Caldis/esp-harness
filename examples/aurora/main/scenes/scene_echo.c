/*
 * Scene VI · Echo — audio peripheral, visualised.
 *
 * Long-press to play a 400 Hz / 600 ms tone through the ES8311 speaker.
 * Visual feedback: three concentric ripples expand outward from the
 * centre on each press, fading as they grow. Each press also nudges
 * the on-screen "press count" so a silent room can still confirm the
 * scene works.
 *
 * Aesthetic: deep teal accent on AMOLED black. Hairline rings, no
 * fill — keeps the visual tight and lets the motion carry the impact.
 */

#include "harness/scene_framework.h"
#include "lvgl.h"
#include "peripherals/audio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdio.h>

#define ACCENT      0x4FD1C5     /* deep teal */
#define RIPPLE_N    3
#define RIPPLE_MAX_R 200
#define TONE_FREQ_HZ 440
#define TONE_DUR_MS  600
#define TONE_VOL_PCT 55

typedef struct {
    lv_obj_t *roman;
    lv_obj_t *hint;
    lv_obj_t *counter;
    lv_obj_t *ripples[RIPPLE_N];
    lv_anim_t  anims[RIPPLE_N];
    uint32_t   press_count;
} echo_state_t;

/* Animation tick for ripple radius. Stored as user_data on the anim. */
static void ripple_size_anim(void *obj, int32_t v)
{
    lv_obj_t *ring = (lv_obj_t *)obj;
    lv_obj_set_size(ring, v * 2, v * 2);
    lv_obj_center(ring);
}

static void ripple_opa_anim(void *obj, int32_t v)
{
    lv_obj_set_style_border_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void start_ripple(lv_obj_t *ring, int delay_ms)
{
    /* Radius from 12 → RIPPLE_MAX_R over 900 ms. */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ring);
    lv_anim_set_values(&a, 12, RIPPLE_MAX_R);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, ripple_size_anim);
    lv_anim_start(&a);

    /* Border opacity from 220 → 0 in parallel. */
    lv_anim_t o;
    lv_anim_init(&o);
    lv_anim_set_var(&o, ring);
    lv_anim_set_values(&o, 220, 0);
    lv_anim_set_duration(&o, 900);
    lv_anim_set_delay(&o, delay_ms);
    lv_anim_set_exec_cb(&o, ripple_opa_anim);
    lv_anim_start(&o);
}

static void echo_long_press(scene_t *s)
{
    echo_state_t *st = (echo_state_t *)s->user_data;
    if (!st) return;

    /* Fire the tone — async, non-blocking. */
    bool ok = audio_play_tone_async(TONE_FREQ_HZ, TONE_DUR_MS, TONE_VOL_PCT);

    /* Ripple animation: stagger three rings by 80 ms each. */
    for (int i = 0; i < RIPPLE_N; ++i) {
        start_ripple(st->ripples[i], i * 80);
    }

    /* Counter chrome — readable evidence even with no speaker. */
    st->press_count++;
    char buf[24];
    snprintf(buf, sizeof(buf), "%lu  pulse%s",
             (unsigned long)st->press_count,
             st->press_count == 1 ? "" : "s");
    lv_label_set_text(st->counter, buf);

    if (!ok) {
        lv_label_set_text(st->hint, "audio not ready");
    }
}

static void echo_init(scene_t *s, lv_obj_t *parent)
{
    echo_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;

    /* Roman VI. */
    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(st->roman, 6, 0);
    lv_obj_set_style_text_color(st->roman, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_60, 0);
    lv_label_set_text(st->roman, "VI");
    lv_obj_align(st->roman, LV_ALIGN_TOP_MID, 0, 110);

    /* Hint. */
    st->hint = lv_label_create(parent);
    lv_obj_set_style_text_font(st->hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->hint, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->hint, LV_OPA_50, 0);
    lv_label_set_text(st->hint, "hold to pulse");
    lv_obj_align(st->hint, LV_ALIGN_CENTER, 0, -8);

    /* Counter. */
    st->counter = lv_label_create(parent);
    lv_obj_set_style_text_font(st->counter, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(st->counter, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->counter, LV_OPA_80, 0);
    lv_label_set_text(st->counter, "0  pulses");
    lv_obj_align(st->counter, LV_ALIGN_CENTER, 0, 30);

    /* Ripple rings — created once, animated on each press. Start at
     * radius=12 with full opacity (invisible because no border-opa
     * until the animation runs the first time). */
    for (int i = 0; i < RIPPLE_N; ++i) {
        lv_obj_t *r = lv_obj_create(parent);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, 24, 24);
        lv_obj_center(r);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(ACCENT), 0);
        lv_obj_set_style_border_width(r, 2, 0);
        lv_obj_set_style_border_opa(r, LV_OPA_0, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
        st->ripples[i] = r;
    }
}

scene_t scene_echo = {
    .id           = "echo",
    .display_name = "VI. Echo",
    .accent       = LV_COLOR_MAKE(0x4F, 0xD1, 0xC5),
    .init         = echo_init,
    .on_long_press = echo_long_press,
};
