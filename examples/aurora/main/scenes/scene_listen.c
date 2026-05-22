/*
 * Scene XII · Listen — microphone + speaker loopback with controls.
 *
 *   - Press and HOLD the screen → record while held. Releasing stops
 *     recording and immediately plays back the captured audio.
 *   - Max recording length is 10 s (≈ 440 KB PSRAM @ 22050 Hz mono
 *     int16). Hardware cap; below that, no constraint.
 *   - BOOT button → volume −5 %. USER button → volume +5 %. The
 *     current volume is shown as a bar and number at the top of the
 *     screen; the value persists across recordings.
 *
 * Visual phases while busy:
 *   idle     → "hold to record + play back"
 *   recording→ "recording 1.4 s" (count-up timer)
 *   playing  → "playing back..."  + final PEAK/RMS dBFS shown
 *   done     → "hold to listen again"
 *
 * The peak / RMS bars sit below the volume widget. dBFS scale runs
 * −60 dB (empty) to 0 dB (full); −20 dB RMS is a normal-voice room.
 */

#include "harness/scene_framework.h"
#include "lvgl.h"
#include "peripherals/audio.h"
#include "peripherals/keys.h"
#include "peripherals/settings.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <math.h>

#define ACCENT      0xE0888A
#define BAR_W       300
#define BAR_H       10
#define VOL_BAR_W   200
#define VOL_BAR_H   10
#define DB_FLOOR    -60.0f
#define MAX_REC_MS  10000
#define VOL_STEP    5

typedef struct {
    /* chrome */
    lv_obj_t *roman;

    /* volume row */
    lv_obj_t *vol_tag;       /* "VOL" */
    lv_obj_t *vol_track;
    lv_obj_t *vol_bar;
    lv_obj_t *vol_value;     /* "70 %" */

    /* state */
    lv_obj_t *state_label;   /* "recording 1.4 s" / "playing back..." */

    /* dB readouts */
    lv_obj_t *peak_label, *peak_track, *peak_bar, *peak_value;
    lv_obj_t *rms_label,  *rms_track,  *rms_bar,  *rms_value;
    lv_obj_t *hint;

    /* live tick (50 ms) — updates volume display + counts boot/user
     * presses for vol± + updates recording duration label. */
    lv_timer_t *timer;

    /* key change-detect counters */
    uint32_t last_boot_count;
    uint32_t last_user_count;

    /* recording state */
    bool busy;                   /* a recording / playback in progress */
    volatile bool stop_request;  /* async task watches this */
    uint32_t record_start_ms;
} listen_state_t;

static int db_to_pct(float db)
{
    if (db <= DB_FLOOR) return 0;
    if (db >= 0.0f)     return 100;
    return (int)((db - DB_FLOOR) * 100.0f / (-DB_FLOOR));
}

static void refresh_vol_display(listen_state_t *st)
{
    int v = audio_get_volume();
    lv_obj_set_width(st->vol_bar, (VOL_BAR_W * v) / 100);
    char buf[12];
    snprintf(buf, sizeof(buf), "%d %%", v);
    lv_label_set_text(st->vol_value, buf);
}

/* ── async glue for loopback phases ──────────────────────────────── */

typedef struct {
    listen_state_t *st;
    audio_loopback_phase_t phase;
    float peak_dbfs;
    float rms_dbfs;
} listen_evt_t;

static void apply_phase_lvgl(void *arg)
{
    listen_evt_t *e = (listen_evt_t *)arg;
    if (!e || !e->st) { lv_free(e); return; }
    listen_state_t *st = e->st;

    switch (e->phase) {
        case AUDIO_LB_RECORDING:
            /* state_label is updated by the live tick while recording —
             * leave it alone here. */
            lv_label_set_text(st->hint, "release to stop");
            break;
        case AUDIO_LB_PLAYING: {
            int pp = db_to_pct(e->peak_dbfs);
            int rp = db_to_pct(e->rms_dbfs);
            lv_obj_set_width(st->peak_bar, (BAR_W * pp) / 100);
            lv_obj_set_width(st->rms_bar,  (BAR_W * rp) / 100);
            char buf[24];
            snprintf(buf, sizeof(buf), "%.0f dB", e->peak_dbfs);
            lv_label_set_text(st->peak_value, buf);
            snprintf(buf, sizeof(buf), "%.0f dB", e->rms_dbfs);
            lv_label_set_text(st->rms_value, buf);
            lv_label_set_text(st->state_label, "playing back...");
            lv_label_set_text(st->hint, "boot/user adjusts volume");
            break;
        }
        case AUDIO_LB_DONE:
            lv_label_set_text(st->state_label, "done");
            lv_label_set_text(st->hint, "hold to record   vol via boot/user");
            st->busy = false;
            break;
    }
    lv_free(e);
}

static void on_phase(audio_loopback_phase_t phase,
                     float peak_dbfs, float rms_dbfs, void *arg)
{
    listen_state_t *st = (listen_state_t *)arg;
    listen_evt_t *e = (listen_evt_t *)lv_malloc_zeroed(sizeof(*e));
    if (!e) return;
    e->st = st;
    e->phase = phase;
    e->peak_dbfs = peak_dbfs;
    e->rms_dbfs  = rms_dbfs;
    lv_async_call(apply_phase_lvgl, e);
}

/* ── gestures ────────────────────────────────────────────────────── */

static void listen_long_press(scene_t *s)
{
    listen_state_t *st = (listen_state_t *)s->user_data;
    if (!st || st->busy) return;
    st->busy = true;
    st->stop_request = false;
    st->record_start_ms = lv_tick_get();
    lv_label_set_text(st->state_label, "recording 0.0 s");
    lv_label_set_text(st->hint, "release to stop");

    if (!audio_loopback_dynamic_async(MAX_REC_MS, &st->stop_request,
                                       on_phase, st)) {
        lv_label_set_text(st->state_label, "audio not ready");
        lv_label_set_text(st->hint, "audio_init likely failed at boot");
        st->busy = false;
    }
}

static void listen_release(scene_t *s)
{
    listen_state_t *st = (listen_state_t *)s->user_data;
    if (!st) return;
    /* If we're mid-recording, signal the audio task to wind down and
     * proceed to playback. Setting the volatile flag is enough — the
     * task polls it after each ~23 ms chunk. */
    if (st->busy) st->stop_request = true;
}

/* ── live tick: volume buttons + recording duration ─────────────── */

static void listen_tick(lv_timer_t *t)
{
    listen_state_t *st = (listen_state_t *)lv_timer_get_user_data(t);
    if (!st) return;

    /* Volume control: BOOT and USER buttons act as -5 / +5 % steppers.
     * We detect rising edges via keys_state count deltas, not levels,
     * so holding doesn't auto-repeat — one tap = one step. */
    keys_state_t k;
    keys_get(&k);
    bool vol_changed = false;
    if (k.boot_count != st->last_boot_count) {
        st->last_boot_count = k.boot_count;
        audio_set_volume(audio_get_volume() - VOL_STEP);
        vol_changed = true;
    }
    if (k.user_count != st->last_user_count) {
        st->last_user_count = k.user_count;
        audio_set_volume(audio_get_volume() + VOL_STEP);
        vol_changed = true;
    }
    if (vol_changed) {
        refresh_vol_display(st);
        settings_set_volume(audio_get_volume());
    }

    /* Recording duration counter — visible feedback while held. */
    if (st->busy && !st->stop_request) {
        uint32_t now = lv_tick_get();
        float secs = (float)(now - st->record_start_ms) / 1000.0f;
        char buf[32];
        if (secs >= (float)MAX_REC_MS / 1000.0f - 0.05f) {
            snprintf(buf, sizeof(buf), "recording %.1f s  max", secs);
        } else {
            snprintf(buf, sizeof(buf), "recording %.1f s", secs);
        }
        lv_label_set_text(st->state_label, buf);
    }
}

/* ── widgets ─────────────────────────────────────────────────────── */

static lv_obj_t *make_track(lv_obj_t *parent, int w, int h, lv_opa_t border_opa)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(o, h / 2, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_opa(o, border_opa, 0);
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
static lv_obj_t *make_tag(lv_obj_t *parent, const char *txt)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(lbl, 3, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(lbl, LV_OPA_70, 0);
    lv_label_set_text(lbl, txt);
    return lbl;
}

static void listen_init(scene_t *s, lv_obj_t *parent)
{
    listen_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;

    /* Roman XII. */
    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(st->roman, 6, 0);
    lv_obj_set_style_text_color(st->roman, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_60, 0);
    lv_label_set_text(st->roman, "XII");
    lv_obj_align(st->roman, LV_ALIGN_TOP_MID, 0, 92);

    /* Volume row: tag / track+bar / value. */
    st->vol_tag = make_tag(parent, "VOL");
    lv_obj_align(st->vol_tag, LV_ALIGN_CENTER, -130, -75);
    st->vol_track = make_track(parent, VOL_BAR_W, VOL_BAR_H, LV_OPA_30);
    lv_obj_align(st->vol_track, LV_ALIGN_CENTER, 0, -75);
    st->vol_bar = make_bar(parent, VOL_BAR_H);
    lv_obj_align_to(st->vol_bar, st->vol_track, LV_ALIGN_LEFT_MID, 0, 0);
    st->vol_value = lv_label_create(parent);
    lv_obj_set_style_text_font(st->vol_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->vol_value, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->vol_value, LV_OPA_90, 0);
    lv_label_set_text(st->vol_value, "-");
    lv_obj_align(st->vol_value, LV_ALIGN_CENTER, 130, -75);

    /* State label. */
    st->state_label = lv_label_create(parent);
    lv_obj_set_style_text_font(st->state_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(st->state_label, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->state_label, LV_OPA_80, 0);
    lv_label_set_text(st->state_label, "idle");
    lv_obj_align(st->state_label, LV_ALIGN_CENTER, 0, -35);

    /* PEAK row. */
    st->peak_label = make_tag(parent, "PEAK");
    lv_obj_align(st->peak_label, LV_ALIGN_CENTER, -180, 5);
    st->peak_track = make_track(parent, BAR_W, BAR_H, LV_OPA_30);
    lv_obj_align(st->peak_track, LV_ALIGN_CENTER, 0, 5);
    st->peak_bar = make_bar(parent, BAR_H);
    lv_obj_align_to(st->peak_bar, st->peak_track, LV_ALIGN_LEFT_MID, 0, 0);
    st->peak_value = lv_label_create(parent);
    lv_obj_set_style_text_font(st->peak_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->peak_value, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->peak_value, LV_OPA_80, 0);
    lv_label_set_text(st->peak_value, "-");
    lv_obj_align(st->peak_value, LV_ALIGN_CENTER, 180, 5);

    /* RMS row. */
    st->rms_label = make_tag(parent, "RMS");
    lv_obj_align(st->rms_label, LV_ALIGN_CENTER, -180, 30);
    st->rms_track = make_track(parent, BAR_W, BAR_H, LV_OPA_30);
    lv_obj_align(st->rms_track, LV_ALIGN_CENTER, 0, 30);
    st->rms_bar = make_bar(parent, BAR_H);
    lv_obj_align_to(st->rms_bar, st->rms_track, LV_ALIGN_LEFT_MID, 0, 0);
    st->rms_value = lv_label_create(parent);
    lv_obj_set_style_text_font(st->rms_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->rms_value, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->rms_value, LV_OPA_80, 0);
    lv_label_set_text(st->rms_value, "-");
    lv_obj_align(st->rms_value, LV_ALIGN_CENTER, 180, 30);

    /* Hint at the bottom. */
    st->hint = lv_label_create(parent);
    lv_obj_set_style_text_font(st->hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->hint, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->hint, LV_OPA_50, 0);
    lv_label_set_text(st->hint, "hold to record   vol via boot/user");
    lv_obj_align(st->hint, LV_ALIGN_CENTER, 0, 70);

    /* Pre-fill key counters from current state so the first vol±
     * detection only fires on actual subsequent presses. */
    keys_state_t k;
    keys_get(&k);
    st->last_boot_count = k.boot_count;
    st->last_user_count = k.user_count;

    /* 50 ms live tick: vol display + button polling + duration counter.
     * Start paused; on_show resumes. */
    st->timer = lv_timer_create(listen_tick, 50, st);
    lv_timer_pause(st->timer);
    refresh_vol_display(st);
}

static void listen_on_show(scene_t *s)
{
    listen_state_t *st = (listen_state_t *)s->user_data;
    if (!st) return;
    /* Resync key counters when entering — otherwise stale Δ between
     * sessions would trigger spurious vol changes. */
    keys_state_t k;
    keys_get(&k);
    st->last_boot_count = k.boot_count;
    st->last_user_count = k.user_count;
    refresh_vol_display(st);
    if (st->timer) lv_timer_resume(st->timer);
}
static void listen_on_hide(scene_t *s)
{
    listen_state_t *st = (listen_state_t *)s->user_data;
    if (st && st->timer) lv_timer_pause(st->timer);
}

scene_t scene_listen = {
    .id           = "listen",
    .display_name  = "XII. Listen",
    .accent        = LV_COLOR_MAKE(0xE0, 0x88, 0x8A),
    .description   = "Press-and-hold mic capture (max 10s), releases plays back; BOOT/USER adjust volume",
    .tags          = "audio,mic,speaker,interactive,loopback",
    .init          = listen_init,
    .on_show       = listen_on_show,
    .on_hide       = listen_on_hide,
    .on_long_press = listen_long_press,
    .on_release    = listen_release,
};
