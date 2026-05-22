/*
 * Scene XVI · Spin — IMU gyroscope live readout.
 *
 * Complement to Scene IV Tilt (which reads the accelerometer). The
 * QMI8658's gyro is already enabled by qmi8658_init at 1 kHz / 512 dps;
 * the imu task just samples + EMAs it. We display three signed-bar
 * meters (pitch / roll / yaw) and a numeric dps readout.
 *
 * Bar scaling: ±250 dps → full width. Past that we clip at the edge
 * to avoid runaway visuals when the user actually spins the board.
 *
 * Static (board flat on table) reading should sit near 0 ±1 dps per
 * axis. A noticeable non-zero baseline indicates fuel-gauge-level
 * drift that a real product would compensate; for a test page it's
 * informative as-is.
 */

#include "harness/scene_framework.h"
#include "lvgl.h"
#include "peripherals/imu.h"
#include "esp_log.h"
#include <stdio.h>
#include <math.h>

#define ACCENT       0xC586DA     /* lavender */
#define BAR_HALF_W   140
#define BAR_H        10
#define DPS_FULL     250.0f       /* full bar width = ±250 dps */

typedef struct {
    lv_obj_t *roman;
    lv_obj_t *hint;
    /* Per-axis row: label / centred track / signed bar / value */
    struct {
        lv_obj_t *label;
        lv_obj_t *track;
        lv_obj_t *bar;
        lv_obj_t *value;
    } row[3];
    lv_timer_t *timer;
} spin_state_t;

static const char *AXIS_NAMES[3] = { "X (roll)", "Y (pitch)", "Z (yaw)" };

static lv_obj_t *make_track(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, BAR_HALF_W * 2, BAR_H);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(o, BAR_H / 2, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_30, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *make_bar(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, 0, BAR_H);
    lv_obj_set_style_radius(o, BAR_H / 2, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_80, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void apply_axis(spin_state_t *st, int i, float dps)
{
    /* Centre line: track's midpoint. Bar grows from centre left or right
     * by |dps| / DPS_FULL * BAR_HALF_W, capped. */
    float clamped = dps;
    if (clamped >  DPS_FULL) clamped =  DPS_FULL;
    if (clamped < -DPS_FULL) clamped = -DPS_FULL;
    int pixels = (int)(fabsf(clamped) / DPS_FULL * (float)BAR_HALF_W);
    if (pixels < 1) pixels = 1;   /* always show a dot at zero */
    lv_obj_set_width(st->row[i].bar, pixels);
    /* Anchor: from centre of the track, then offset half the bar width
     * left or right depending on sign. */
    lv_align_t a = (clamped >= 0) ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID;
    /* Convert centre-anchor into left-anchor offset. */
    if (clamped >= 0) {
        lv_obj_align_to(st->row[i].bar, st->row[i].track, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_translate_x(st->row[i].bar, pixels / 2, 0);
    } else {
        lv_obj_align_to(st->row[i].bar, st->row[i].track, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_translate_x(st->row[i].bar, -pixels / 2, 0);
    }
    (void)a;

    char buf[16];
    snprintf(buf, sizeof(buf), "%+.1f dps", dps);
    lv_label_set_text(st->row[i].value, buf);
}

static void spin_tick(lv_timer_t *t)
{
    spin_state_t *st = (spin_state_t *)lv_timer_get_user_data(t);
    if (!st) return;
    float gx, gy, gz;
    imu_get_gyro(&gx, &gy, &gz);
    apply_axis(st, 0, gx);
    apply_axis(st, 1, gy);
    apply_axis(st, 2, gz);
}

static void spin_init(scene_t *s, lv_obj_t *parent)
{
    spin_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;

    /* Roman XVI */
    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(st->roman, 6, 0);
    lv_obj_set_style_text_color(st->roman, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_60, 0);
    lv_label_set_text(st->roman, "XVI");
    lv_obj_align(st->roman, LV_ALIGN_TOP_MID, 0, 100);

    int y = -55;
    for (int i = 0; i < 3; ++i) {
        st->row[i].label = lv_label_create(parent);
        lv_obj_set_style_text_font(st->row[i].label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(st->row[i].label, lv_color_hex(ACCENT), 0);
        lv_obj_set_style_text_opa(st->row[i].label, LV_OPA_70, 0);
        lv_label_set_text(st->row[i].label, AXIS_NAMES[i]);
        lv_obj_align(st->row[i].label, LV_ALIGN_CENTER, -180, y);

        st->row[i].track = make_track(parent);
        lv_obj_align(st->row[i].track, LV_ALIGN_CENTER, 0, y);
        st->row[i].bar = make_bar(parent);
        lv_obj_align_to(st->row[i].bar, st->row[i].track, LV_ALIGN_CENTER, 0, 0);

        st->row[i].value = lv_label_create(parent);
        lv_obj_set_style_text_font(st->row[i].value, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(st->row[i].value, lv_color_hex(ACCENT), 0);
        lv_obj_set_style_text_opa(st->row[i].value, LV_OPA_80, 0);
        lv_label_set_text(st->row[i].value, "+0.0 dps");
        lv_obj_align(st->row[i].value, LV_ALIGN_CENTER, 185, y);

        y += 45;
    }

    /* Hint */
    st->hint = lv_label_create(parent);
    lv_obj_set_style_text_font(st->hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->hint, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->hint, LV_OPA_50, 0);
    lv_label_set_text(st->hint, "rotate the board to see live gyro");
    lv_obj_align(st->hint, LV_ALIGN_CENTER, 0, 100);

    /* 50 ms refresh = 20 Hz. Matches the IMU EMA cadence. */
    st->timer = lv_timer_create(spin_tick, 50, st);
    lv_timer_pause(st->timer);
}

static void spin_on_show(scene_t *s)
{
    spin_state_t *st = (spin_state_t *)s->user_data;
    if (st && st->timer) {
        lv_timer_resume(st->timer);
        spin_tick(st->timer);
    }
}
static void spin_on_hide(scene_t *s)
{
    spin_state_t *st = (spin_state_t *)s->user_data;
    if (st && st->timer) lv_timer_pause(st->timer);
}

scene_t scene_spin = {
    .id           = "spin",
    .display_name = "XVI. Spin",
    .accent       = LV_COLOR_MAKE(0xC5, 0x86, 0xDA),
    .description  = "IMU gyroscope live: pitch / roll / yaw signed bars + numeric dps",
    .tags         = "imu,gyro,live,dashboard",
    .init         = spin_init,
    .on_show      = spin_on_show,
    .on_hide      = spin_on_hide,
};
