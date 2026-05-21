/*
 * Scene XIII · Tone — speaker test, no microphone involvement.
 *
 * A dedicated page for exercising just the playback chain (ES8311 +
 * NS-class-D PA + speaker), separate from Listen which mixes in the
 * record path. Same volume mechanism as Listen: BOOT decreases by 5,
 * USER increases by 5, current value shown as a bar at the top.
 *
 * Long press = play a 600 ms sine at the currently-selected frequency,
 * then advance to the next preset. Five presets sweep the speaker's
 * usable band:
 *
 *   220 Hz   — low (most speakers struggle here; if you hear it, good)
 *   440 Hz   — A4, classic reference
 *   1000 Hz  — sweet spot for small-driver speakers
 *   2000 Hz  — upper mid
 *   4000 Hz  — high (sharp / piercing; bright speakers go brassy)
 *
 * No mic, no PSRAM allocation, no record-then-play — this is the
 * simplest possible "is the speaker alive" surface.
 */

#include "harness/scene_framework.h"
#include "lvgl.h"
#include "peripherals/audio.h"
#include "peripherals/keys.h"
#include "peripherals/settings.h"
#include "esp_log.h"
#include <stdio.h>

#define ACCENT       0xF8C77E    /* warm honey */
#define VOL_BAR_W    200
#define VOL_BAR_H    10
#define TONE_DUR_MS  600
#define VOL_STEP     5

static const int s_freqs[] = { 220, 440, 1000, 2000, 4000 };
#define N_FREQS  (sizeof(s_freqs) / sizeof(s_freqs[0]))

typedef struct {
    lv_obj_t *roman;
    lv_obj_t *vol_tag;
    lv_obj_t *vol_track;
    lv_obj_t *vol_bar;
    lv_obj_t *vol_value;
    lv_obj_t *freq_big;       /* "1000 Hz" huge */
    lv_obj_t *state_label;    /* "playing 1000 Hz" / "ready" */
    lv_obj_t *hint;
    lv_timer_t *timer;        /* 50 ms tick for vol button polling */
    uint32_t last_boot_count;
    uint32_t last_user_count;
    int      preset_idx;
    bool     playing;
    uint32_t play_until_ms;
} tone_state_t;

static void refresh_vol(tone_state_t *st)
{
    int v = audio_get_volume();
    lv_obj_set_width(st->vol_bar, (VOL_BAR_W * v) / 100);
    char buf[12];
    snprintf(buf, sizeof(buf), "%d %%", v);
    lv_label_set_text(st->vol_value, buf);
}

static void refresh_freq(tone_state_t *st)
{
    char buf[16];
    int hz = s_freqs[st->preset_idx];
    if (hz >= 1000) {
        snprintf(buf, sizeof(buf), "%d.%d kHz", hz / 1000, (hz % 1000) / 100);
    } else {
        snprintf(buf, sizeof(buf), "%d Hz", hz);
    }
    lv_label_set_text(st->freq_big, buf);
}

static void tone_long_press(scene_t *s)
{
    tone_state_t *st = (tone_state_t *)s->user_data;
    if (!st) return;
    int hz = s_freqs[st->preset_idx];
    /* Use the user-selected volume; async playback so the UI tick
     * keeps updating during the tone. */
    int vol = audio_get_volume();
    bool ok = audio_play_tone_async(hz, TONE_DUR_MS, vol);

    if (!ok) {
        lv_label_set_text(st->state_label, "audio not ready");
        lv_label_set_text(st->hint, "audio_init likely failed at boot");
        return;
    }
    st->playing = true;
    st->play_until_ms = lv_tick_get() + TONE_DUR_MS;
    char buf[40];
    snprintf(buf, sizeof(buf), "playing %d Hz @ %d %%", hz, vol);
    lv_label_set_text(st->state_label, buf);
    lv_label_set_text(st->hint, "long-press cycles to next preset");

    /* Advance to next preset for the *next* long-press. */
    st->preset_idx = (st->preset_idx + 1) % (int)N_FREQS;
    refresh_freq(st);
}

static void tone_tick(lv_timer_t *t)
{
    tone_state_t *st = (tone_state_t *)lv_timer_get_user_data(t);
    if (!st) return;

    /* Volume via BOOT (-) / USER (+), same edge-counting as Listen. */
    keys_state_t k;
    keys_get(&k);
    bool changed = false;
    if (k.boot_count != st->last_boot_count) {
        st->last_boot_count = k.boot_count;
        audio_set_volume(audio_get_volume() - VOL_STEP);
        changed = true;
    }
    if (k.user_count != st->last_user_count) {
        st->last_user_count = k.user_count;
        audio_set_volume(audio_get_volume() + VOL_STEP);
        changed = true;
    }
    if (changed) {
        refresh_vol(st);
        settings_set_volume(audio_get_volume());
    }

    /* Auto-clear "playing" message once the tone duration has elapsed. */
    if (st->playing && (int32_t)(st->play_until_ms - lv_tick_get()) <= 0) {
        st->playing = false;
        lv_label_set_text(st->state_label, "ready");
    }
}

static lv_obj_t *make_track(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(o, h / 2, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_30, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}
static lv_obj_t *make_bar(lv_obj_t *parent, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, 0, h);
    lv_obj_set_style_radius(o, h / 2, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_90, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void tone_init(scene_t *s, lv_obj_t *parent)
{
    tone_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;
    st->preset_idx = 1;   /* start at 440 Hz */

    /* Roman XIII. */
    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(st->roman, 6, 0);
    lv_obj_set_style_text_color(st->roman, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_60, 0);
    lv_label_set_text(st->roman, "XIII");
    lv_obj_align(st->roman, LV_ALIGN_TOP_MID, 0, 92);

    /* VOL row. */
    st->vol_tag = lv_label_create(parent);
    lv_obj_set_style_text_font(st->vol_tag, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(st->vol_tag, 3, 0);
    lv_obj_set_style_text_color(st->vol_tag, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->vol_tag, LV_OPA_70, 0);
    lv_label_set_text(st->vol_tag, "VOL");
    lv_obj_align(st->vol_tag, LV_ALIGN_CENTER, -130, -70);

    st->vol_track = make_track(parent, VOL_BAR_W, VOL_BAR_H);
    lv_obj_align(st->vol_track, LV_ALIGN_CENTER, 0, -70);
    st->vol_bar = make_bar(parent, VOL_BAR_H);
    lv_obj_align_to(st->vol_bar, st->vol_track, LV_ALIGN_LEFT_MID, 0, 0);

    st->vol_value = lv_label_create(parent);
    lv_obj_set_style_text_font(st->vol_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->vol_value, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->vol_value, LV_OPA_90, 0);
    lv_label_set_text(st->vol_value, "-");
    lv_obj_align(st->vol_value, LV_ALIGN_CENTER, 130, -70);

    /* Big frequency display in the centre. */
    st->freq_big = lv_label_create(parent);
    lv_obj_set_style_text_font(st->freq_big, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(st->freq_big, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->freq_big, LV_OPA_90, 0);
    lv_label_set_text(st->freq_big, "440 Hz");
    lv_obj_align(st->freq_big, LV_ALIGN_CENTER, 0, -10);

    /* State label below frequency. */
    st->state_label = lv_label_create(parent);
    lv_obj_set_style_text_font(st->state_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(st->state_label, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->state_label, LV_OPA_70, 0);
    lv_label_set_text(st->state_label, "ready");
    lv_obj_align(st->state_label, LV_ALIGN_CENTER, 0, 35);

    /* Hint. */
    st->hint = lv_label_create(parent);
    lv_obj_set_style_text_font(st->hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->hint, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->hint, LV_OPA_50, 0);
    lv_label_set_text(st->hint, "hold to play   boot/user = vol");
    lv_obj_align(st->hint, LV_ALIGN_CENTER, 0, 70);

    /* Sync key counters so the first detection only fires on actual
     * subsequent presses. */
    keys_state_t k;
    keys_get(&k);
    st->last_boot_count = k.boot_count;
    st->last_user_count = k.user_count;

    refresh_freq(st);
    refresh_vol(st);

    st->timer = lv_timer_create(tone_tick, 50, st);
    lv_timer_pause(st->timer);
}

static void tone_on_show(scene_t *s)
{
    tone_state_t *st = (tone_state_t *)s->user_data;
    if (!st) return;
    keys_state_t k;
    keys_get(&k);
    st->last_boot_count = k.boot_count;
    st->last_user_count = k.user_count;
    refresh_vol(st);
    if (st->timer) lv_timer_resume(st->timer);
}
static void tone_on_hide(scene_t *s)
{
    tone_state_t *st = (tone_state_t *)s->user_data;
    if (st && st->timer) lv_timer_pause(st->timer);
}

scene_t scene_tone = {
    .id           = "tone",
    .display_name = "XIII. Tone",
    .accent       = LV_COLOR_MAKE(0xF8, 0xC7, 0x7E),
    .init         = tone_init,
    .on_show      = tone_on_show,
    .on_hide      = tone_on_hide,
    .on_long_press = tone_long_press,
};
