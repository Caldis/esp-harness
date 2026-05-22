/*
 * Scene X · Cell — battery dashboard.
 *
 * Aurora's first "informational" rather than "ambient" screen. Five
 * data fields, no chrome animation. Designed to be readable at arm's
 * length, AMOLED-optimised (deep black, single accent rotating by
 * charge state).
 *
 * Layout (centred on 480 × 480):
 *
 *                     X
 *                ╭───────╮
 *               │   81%   │       ← huge percent in centre
 *                ╰───────╯
 *
 *                 4.05 V          ← battery voltage
 *               USB 5.12 V        ← VBUS voltage (only when plugged)
 *              charging · fast    ← state text
 *               +0.32 %/min       ← derived rate, ~28 min to full
 *
 *  ⬤ … indicator dots …
 *
 * Accent colour mapping:
 *   charging (any phase, % > 15)   → cyan
 *   charged                        → cool white
 *   discharging on battery, % ≥ 30 → warm amber
 *   discharging, 15 ≤ % < 30       → soft orange
 *   any state with % < 15          → soft alert red
 *
 * Arc fill = battery percent. 270° sweep starting at the bottom so
 * the gap reads as "empty downward".
 *
 * Refresh: 4 Hz (250 ms) — fast enough that "+0.32 %/min" feels live
 * but slow enough that the AMOLED pixels stay calm.
 */

#include "harness/scene_framework.h"
#include "lvgl.h"
#include "peripherals/pmic.h"
#include "esp_log.h"
#include <stdio.h>
#include <math.h>

#define ARC_OUTER_R   190
#define ARC_WIDTH      8

typedef struct {
    lv_obj_t *roman;
    lv_obj_t *arc;
    lv_obj_t *pct_big;     /* "81%" huge */
    lv_obj_t *v_bat;       /* "4.05 V" */
    lv_obj_t *v_bus;       /* "USB 5.12 V" or "" */
    lv_obj_t *state;       /* "charging fast" */
    lv_obj_t *rate;        /* "+0.32 %/min · ~28 min" */
    lv_timer_t *timer;
} cell_state_t;

static lv_color_t accent_for(const pmic_state_t *p)
{
    if (!p->battery)              return lv_color_hex(0x707070);   /* grey */
    if (p->percent >= 0 && p->percent < 15)
                                  return lv_color_hex(0xFF4060);   /* low alert red */
    if (p->charge == PMIC_CHG_DONE)
                                  return lv_color_hex(0xE4ECF5);   /* cool white */
    if (p->charge == PMIC_CHG_TRICKLE ||
        p->charge == PMIC_CHG_PRE ||
        p->charge == PMIC_CHG_CONST_CUR ||
        p->charge == PMIC_CHG_CONST_VOLT)
                                  return lv_color_hex(0x4DD4FF);   /* ice cyan */
    if (p->percent >= 30)         return lv_color_hex(0xFFB070);   /* warm amber */
    return lv_color_hex(0xFF8849);                                  /* soft orange */
}

static const char *state_phrase(const pmic_state_t *p)
{
    if (!p->battery)                                       return "no battery";
    if (p->charge == PMIC_CHG_DONE)                        return "charged";
    if (p->charge == PMIC_CHG_TRICKLE)                     return "charging trickle";
    if (p->charge == PMIC_CHG_PRE)                         return "charging precharge";
    if (p->charge == PMIC_CHG_CONST_CUR)                   return "charging fast";
    if (p->charge == PMIC_CHG_CONST_VOLT)                  return "charging taper";
    if (p->vbus_in)                                        return "USB attached";
    return "on battery";
}

/* Build the rate text. AXP2101's fuel gauge reports integer percent
 * and is sticky for minutes between steps, so %/min is often 0 even
 * when the chip is actively charging — voltage rate is the responsive
 * fallback. Layout:
 *
 *   no battery        → ""
 *   |%/min| ≥ 0.05    → "+0.32 %/min · full in ~28 min"
 *   else              → "+18 mV/min" (raw voltage trend) */
static void format_rate(char *buf, size_t cap, const pmic_state_t *p)
{
    if (!p->battery) { buf[0] = '\0'; return; }
    float pr = p->rate_pct_per_min;
    if (fabsf(pr) >= 0.05f) {
        if (pr > 0.0f) {
            int remaining = 100 - (p->percent < 0 ? 0 : p->percent);
            int min_to_full = (int)((float)remaining / pr);
            if (min_to_full >= 60) {
                snprintf(buf, cap, "+%.2f %%/min  full ~%dh%02d",
                         pr, min_to_full / 60, min_to_full % 60);
            } else {
                snprintf(buf, cap, "+%.2f %%/min  full ~%d min", pr, min_to_full);
            }
        } else {
            int pct = p->percent < 0 ? 0 : p->percent;
            int min_to_empty = (int)((float)pct / -pr);
            if (min_to_empty >= 60) {
                snprintf(buf, cap, "%.2f %%/min  empty ~%dh%02d",
                         pr, min_to_empty / 60, min_to_empty % 60);
            } else {
                snprintf(buf, cap, "%.2f %%/min  empty ~%d min", pr, min_to_empty);
            }
        }
        return;
    }
    /* %/min is sticky — fall back to mV/min trend. */
    float vr = p->rate_mv_per_min;
    if (fabsf(vr) >= 0.5f) {
        snprintf(buf, cap, "%+.0f mV/min", vr);
    } else {
        snprintf(buf, cap, "stable");
    }
}

static void cell_tick(lv_timer_t *t)
{
    cell_state_t *st = (cell_state_t *)lv_timer_get_user_data(t);
    if (!st) return;
    pmic_state_t p;
    pmic_get(&p);
    lv_color_t c = accent_for(&p);

    /* Arc fill */
    int v = p.battery && p.percent >= 0 ? p.percent : 0;
    lv_arc_set_value(st->arc, v);
    lv_obj_set_style_arc_color(st->arc, c, LV_PART_INDICATOR);

    /* Percent — large central number */
    char buf[40];
    if (p.battery && p.percent >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", p.percent);
    } else {
        snprintf(buf, sizeof(buf), "-");
    }
    lv_label_set_text(st->pct_big, buf);
    lv_obj_set_style_text_color(st->pct_big, c, 0);
    lv_obj_set_style_text_color(st->roman, c, 0);

    /* Battery voltage */
    if (p.battery && p.voltage_mv > 0) {
        snprintf(buf, sizeof(buf), "%.3f V", (float)p.voltage_mv / 1000.0f);
    } else {
        buf[0] = '\0';
    }
    lv_label_set_text(st->v_bat, buf);
    lv_obj_set_style_text_color(st->v_bat, c, 0);

    /* VBUS voltage — only render line when actually attached. */
    if (p.vbus_in) {
        if (p.vbus_voltage_mv > 100) {
            snprintf(buf, sizeof(buf), "USB %.2f V",
                     (float)p.vbus_voltage_mv / 1000.0f);
        } else {
            /* VBUS detected but ADC channel didn't return a usable value
             * yet — show plug status only. */
            snprintf(buf, sizeof(buf), "USB attached");
        }
    } else {
        snprintf(buf, sizeof(buf), "USB off");
    }
    lv_label_set_text(st->v_bus, buf);

    /* State phrase. */
    lv_label_set_text(st->state, state_phrase(&p));

    /* Rate + ETA. */
    format_rate(buf, sizeof(buf), &p);
    lv_label_set_text(st->rate, buf);
}

static void cell_init(scene_t *s, lv_obj_t *parent)
{
    cell_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;

    /* Roman X */
    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(st->roman, 6, 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_60, 0);
    lv_label_set_text(st->roman, "X");
    lv_obj_align(st->roman, LV_ALIGN_TOP_MID, 0, 100);

    /* Arc — battery percent. 270° sweep with the empty wedge pointing
     * down, so glance reads as "missing energy from the bottom". */
    st->arc = lv_arc_create(parent);
    lv_obj_set_size(st->arc, ARC_OUTER_R * 2, ARC_OUTER_R * 2);
    lv_obj_center(st->arc);
    lv_arc_set_rotation(st->arc, 135);
    lv_arc_set_bg_angles(st->arc, 0, 270);
    lv_arc_set_range(st->arc, 0, 100);
    lv_arc_set_value(st->arc, 0);
    lv_obj_remove_style(st->arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(st->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(st->arc, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(st->arc, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_arc_color(st->arc, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
    lv_obj_set_style_arc_width(st->arc, ARC_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(st->arc, lv_color_hex(0x4DD4FF), LV_PART_INDICATOR);

    /* Big percent in centre. */
    st->pct_big = lv_label_create(parent);
    lv_obj_set_style_text_font(st->pct_big, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_letter_space(st->pct_big, 1, 0);
    lv_label_set_text(st->pct_big, "-");
    lv_obj_align(st->pct_big, LV_ALIGN_CENTER, 0, -40);

    /* Battery voltage. */
    st->v_bat = lv_label_create(parent);
    lv_obj_set_style_text_font(st->v_bat, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_opa(st->v_bat, LV_OPA_90, 0);
    lv_label_set_text(st->v_bat, "");
    lv_obj_align(st->v_bat, LV_ALIGN_CENTER, 0, 0);

    /* VBUS voltage. */
    st->v_bus = lv_label_create(parent);
    lv_obj_set_style_text_font(st->v_bus, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->v_bus, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_opa(st->v_bus, LV_OPA_60, 0);
    lv_label_set_text(st->v_bus, "");
    lv_obj_align(st->v_bus, LV_ALIGN_CENTER, 0, 24);

    /* State phrase. */
    st->state = lv_label_create(parent);
    lv_obj_set_style_text_font(st->state, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(st->state, lv_color_hex(0xEEEEEE), 0);
    lv_obj_set_style_text_opa(st->state, LV_OPA_80, 0);
    lv_label_set_text(st->state, "-");
    lv_obj_align(st->state, LV_ALIGN_CENTER, 0, 50);

    /* Rate. */
    st->rate = lv_label_create(parent);
    lv_obj_set_style_text_font(st->rate, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->rate, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_opa(st->rate, LV_OPA_60, 0);
    lv_label_set_text(st->rate, "");
    lv_obj_align(st->rate, LV_ALIGN_CENTER, 0, 75);

    /* 250 ms refresh — feels live, low render cost. Start paused;
     * on_show resumes when this scene becomes active. */
    st->timer = lv_timer_create(cell_tick, 250, st);
    lv_timer_pause(st->timer);
    /* Initial paint with current snapshot. */
    cell_tick(st->timer);
}

static void cell_on_show(scene_t *s)
{
    cell_state_t *st = (cell_state_t *)s->user_data;
    if (st && st->timer) {
        lv_timer_resume(st->timer);
        cell_tick(st->timer);   /* immediate refresh */
    }
}
static void cell_on_hide(scene_t *s)
{
    cell_state_t *st = (cell_state_t *)s->user_data;
    if (st && st->timer) lv_timer_pause(st->timer);
}

scene_t scene_cell = {
    .id           = "cell",
    .display_name = "X. Cell",
    .accent       = LV_COLOR_MAKE(0x4D, 0xD4, 0xFF),
    .description  = "Battery dashboard: percent / voltage / charge-state / VBUS, accent shifts with state",
    .tags         = "pmic,battery,dashboard,readonly",
    .init         = cell_init,
    .on_show      = cell_on_show,
    .on_hide      = cell_on_hide,
};
