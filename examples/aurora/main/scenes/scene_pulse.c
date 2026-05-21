/*
 * Scene V · Pulse — battery / charge state visualisation.
 *
 * A soft breathing ring at the centre of the screen. Two channels of
 * information are encoded:
 *
 *   • SCALE       — the ring's outer radius oscillates between a "rest"
 *                   value (low side of breath) and a "peak" value (high
 *                   side). Breath PERIOD is fixed at 3 s — a calm,
 *                   meditative cadence, independent of battery state.
 *
 *   • COLOUR      — single accent picked from the charge state:
 *                     fast / pre / trickle / taper  → ice cyan  (charging)
 *                     done                          → cool white (full)
 *                     off  (discharging on USB)     → warm amber
 *                     off  (battery <15%)           → soft alert red
 *
 *   • CORE BRIGHT — the inner solid disc's *opacity* maps to charge
 *                   percent. 0% → invisible; 100% → fully opaque. So at
 *                   a glance: ring colour says what's happening, core
 *                   fullness says how much there is.
 *
 * Two small text labels: roman "V" in upper third, percent + state
 * label in lower third.
 *
 * No IMU, no touch — read-only visualisation. Renders cheap (only the
 * dot opacity + ring style update; LVGL invalidates a tight area).
 */

#include "harness/scene_framework.h"
#include "lvgl.h"
#include "peripherals/pmic.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>

#define BREATH_PERIOD_MS  3000
#define R_REST            150
#define R_PEAK            190
#define CORE_R            70

typedef struct {
    lv_obj_t  *roman;       /* "V" upper */
    lv_obj_t  *ring;        /* breathing ring */
    lv_obj_t  *core;        /* solid centre, opacity = battery % */
    lv_obj_t  *pct_label;   /* "72 %" */
    lv_obj_t  *state_label; /* "charging fast" */
    lv_timer_t *timer;
    uint32_t   t0_ms;
    pmic_charge_state_t last_state;
    int        last_percent;
} pulse_state_t;

static lv_color_t ring_colour(pmic_charge_state_t s, int pct, bool vbus)
{
    if (pct >= 0 && pct < 15 && !vbus) {
        return lv_color_hex(0xFF4060);          /* soft alert red */
    }
    switch (s) {
        case PMIC_CHG_DONE:        return lv_color_hex(0xE4ECF5);  /* cool white */
        case PMIC_CHG_CONST_CUR:
        case PMIC_CHG_CONST_VOLT:
        case PMIC_CHG_PRE:
        case PMIC_CHG_TRICKLE:     return lv_color_hex(0x4DD4FF);  /* ice cyan */
        case PMIC_CHG_OFF:
        case PMIC_CHG_NA:
        default:                   return lv_color_hex(0xFFB070);  /* warm amber */
    }
}

static const char *state_text(pmic_state_t *st)
{
    if (!st->battery)            return "no battery";
    if (st->vbus_in && st->charge == PMIC_CHG_DONE)   return "charged";
    if (st->vbus_in && st->charge == PMIC_CHG_CONST_CUR)  return "charging fast";
    if (st->vbus_in && st->charge == PMIC_CHG_CONST_VOLT) return "charging taper";
    if (st->vbus_in && st->charge == PMIC_CHG_PRE)    return "charging pre";
    if (st->vbus_in && st->charge == PMIC_CHG_TRICKLE) return "charging trickle";
    if (st->vbus_in)             return "USB idle";
    return "on battery";
}

static void pulse_tick(lv_timer_t *t)
{
    pulse_state_t *st = (pulse_state_t *)lv_timer_get_user_data(t);
    if (!st) return;

    pmic_state_t pm;
    pmic_get(&pm);

    /* Breathing scale: sinusoidal between R_REST and R_PEAK. */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    float phase = (float)((now_ms - st->t0_ms) % BREATH_PERIOD_MS)
                  / (float)BREATH_PERIOD_MS;
    float k = 0.5f - 0.5f * cosf(phase * 2.0f * (float)M_PI);  /* 0..1 */
    int radius = R_REST + (int)((R_PEAK - R_REST) * k);
    lv_obj_set_size(st->ring, radius * 2, radius * 2);
    lv_obj_center(st->ring);

    /* Update colour / labels only when state or percent changed,
     * to keep invalidations cheap. */
    if (pm.charge != st->last_state || pm.percent != st->last_percent) {
        lv_color_t c = ring_colour(pm.charge, pm.percent, pm.vbus_in);
        lv_obj_set_style_border_color(st->ring, c, 0);
        lv_obj_set_style_text_color(st->pct_label, c, 0);
        lv_obj_set_style_text_color(st->state_label, c, 0);
        lv_obj_set_style_text_color(st->roman, c, 0);
        lv_obj_set_style_bg_color(st->core, c, 0);

        /* Core opacity scales with percent. Render a tiny sliver at 1 %
         * so the centre isn't completely dark — but stay fully
         * invisible when there's no battery (percent < 0). */
        int opa = 0;
        if (pm.percent > 0) {
            opa = 40 + (pm.percent * 175 / 100);   /* 40..215 */
        }
        lv_obj_set_style_bg_opa(st->core, (lv_opa_t)opa, 0);

        char buf[16];
        if (pm.battery && pm.percent >= 0) {
            snprintf(buf, sizeof(buf), "%d%%", pm.percent);
        } else {
            snprintf(buf, sizeof(buf), "-");
        }
        lv_label_set_text(st->pct_label, buf);
        lv_label_set_text(st->state_label, state_text(&pm));

        st->last_state   = pm.charge;
        st->last_percent = pm.percent;
    }
}

static void pulse_init(scene_t *s, lv_obj_t *parent)
{
    pulse_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;
    st->t0_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    st->last_state = PMIC_CHG_NA;
    st->last_percent = -2;   /* sentinel to force first paint */

    /* Roman numeral V. */
    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(st->roman, 6, 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_50, 0);
    lv_label_set_text(st->roman, "V");
    lv_obj_align(st->roman, LV_ALIGN_TOP_MID, 0, 110);

    /* Breathing ring. */
    st->ring = lv_obj_create(parent);
    lv_obj_remove_style_all(st->ring);
    lv_obj_set_size(st->ring, R_REST * 2, R_REST * 2);
    lv_obj_center(st->ring);
    lv_obj_set_style_bg_opa(st->ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(st->ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(st->ring, 2, 0);
    lv_obj_set_style_border_opa(st->ring, LV_OPA_70, 0);
    lv_obj_clear_flag(st->ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(st->ring, LV_OBJ_FLAG_CLICKABLE);

    /* Solid core. */
    st->core = lv_obj_create(parent);
    lv_obj_remove_style_all(st->core);
    lv_obj_set_size(st->core, CORE_R * 2, CORE_R * 2);
    lv_obj_center(st->core);
    lv_obj_set_style_radius(st->core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(st->core, 0, 0);
    lv_obj_set_style_bg_opa(st->core, LV_OPA_TRANSP, 0);   /* set in tick */
    lv_obj_clear_flag(st->core, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(st->core, LV_OBJ_FLAG_CLICKABLE);

    /* Percent label below ring. */
    st->pct_label = lv_label_create(parent);
    lv_obj_set_style_text_font(st->pct_label, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(st->pct_label, 4, 0);
    lv_label_set_text(st->pct_label, "-");
    lv_obj_align(st->pct_label, LV_ALIGN_CENTER, 0, R_PEAK + 35);

    /* State label below percent. */
    st->state_label = lv_label_create(parent);
    lv_obj_set_style_text_font(st->state_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_opa(st->state_label, LV_OPA_70, 0);
    lv_label_set_text(st->state_label, "");
    lv_obj_align(st->state_label, LV_ALIGN_CENTER, 0, R_PEAK + 65);

    /* 30 Hz update — sinusoid is smooth, label is cheap (gated).
     * Start PAUSED. The framework's on_show fires when this scene
     * becomes the active one (auto-shown scenes get on_show too via
     * scene_fw_show()), at which point we resume the tick. */
    st->timer = lv_timer_create(pulse_tick, 33, st);
    lv_timer_pause(st->timer);
}

/* Pause the timer when we're not visible. Without this, the timer keeps
 * invalidating widgets that are inside a HIDDEN container, and after a
 * scene-switch the invalidate queue eventually wedges LVGL — every
 * subsequent LVGL API call blocks. Same fix in scene_tilt. */
static void pulse_on_show(scene_t *s)
{
    pulse_state_t *st = (pulse_state_t *)s->user_data;
    if (st && st->timer) lv_timer_resume(st->timer);
}
static void pulse_on_hide(scene_t *s)
{
    pulse_state_t *st = (pulse_state_t *)s->user_data;
    if (st && st->timer) lv_timer_pause(st->timer);
}

scene_t scene_pulse = {
    .id           = "pulse",
    .display_name = "V. Pulse",
    .accent       = LV_COLOR_MAKE(0x4D, 0xD4, 0xFF),
    .description  = "Breathing ring; colour from charge state, opacity from percent",
    .tags         = "pmic,battery,animation",
    .init         = pulse_init,
    .on_show      = pulse_on_show,
    .on_hide      = pulse_on_hide,
};
