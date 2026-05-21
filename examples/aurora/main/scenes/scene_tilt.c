/*
 * Scene IV · Tilt — IMU-driven spirit level.
 *
 * A small bright dot sits near the centre of the screen. When the device
 * is held flat, it stays put. Tilt the device and the dot rolls toward
 * the "downhill" direction — exactly as a real bubble level would, but
 * inverted (so the dot tracks gravity rather than air bubble).
 *
 * A faint outline ring at fixed radius gives a reference frame, so even
 * when the dot is far off centre it's clearly inside a contained world.
 *
 * Visual identity: pale violet on AMOLED black, restrained.
 *
 * Refresh: 20 Hz (50 ms timer). The screen invalidation is small (the dot
 * is ~14 px) so this is well within the QSPI bandwidth budget even
 * without the v0.2 BSP fix.
 */

#include "harness/scene_framework.h"
#include "lvgl.h"
#include "peripherals/imu.h"
#include "esp_log.h"
#include <math.h>

#define ACCENT   0xB084EE       /* pale violet */
#define DOT_PX   14
#define TRAVEL_PX  150          /* max displacement from centre */
#define REF_RING_R 175          /* the static reference ring radius */
#define DAMP_ALPHA 0.18f        /* visual smoothing on top of IMU EMA */

typedef struct {
    lv_obj_t *dot;
    lv_obj_t *ref;
    lv_obj_t *roman;
    lv_timer_t *timer;
    /* Smoothed pose in screen pixels relative to centre. */
    float dx;
    float dy;
} tilt_state_t;

static void tilt_tick(lv_timer_t *t)
{
    tilt_state_t *st = (tilt_state_t *)lv_timer_get_user_data(t);
    if (!st || !st->dot) return;

    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    if (imu_is_ready()) {
        imu_get_accel(&ax, &ay, &az);
    }

    /* Map gravity (in g, ~±1.0 horizontal range) to a screen offset.
     *
     * Coordinate convention to match the user's intuition when holding
     * the board upright (USB at the bottom):
     *   tilt LEFT  → dot drifts LEFT  (gravity X negative → dx negative)
     *   tilt FORWARD (top edge down) → dot drifts UP   (gravity Y positive → dy negative)
     *
     * The empirically-correct sign mapping for THIS board orientation
     * will be tuned by hand once we can run the test — left here as
     * "natural" and adjusted in commit. */
    float target_dx = -ax * TRAVEL_PX;
    float target_dy =  ay * TRAVEL_PX;

    /* clamp to reference ring radius so the dot never escapes the world */
    float mag = sqrtf(target_dx * target_dx + target_dy * target_dy);
    if (mag > TRAVEL_PX) {
        target_dx *= TRAVEL_PX / mag;
        target_dy *= TRAVEL_PX / mag;
    }

    /* visual EMA so motion looks fluid even at 20 Hz */
    st->dx = st->dx * (1.0f - DAMP_ALPHA) + target_dx * DAMP_ALPHA;
    st->dy = st->dy * (1.0f - DAMP_ALPHA) + target_dy * DAMP_ALPHA;

    /* lv_obj_set_pos sets the top-left corner. To centre a DOT_PX×DOT_PX
     * dot at (233 + dx, 233 + dy), the top-left goes at
     * (233 - DOT_PX/2 + dx, 233 - DOT_PX/2 + dy). */
    lv_obj_set_pos(st->dot,
                   240 - DOT_PX / 2 + (int)st->dx,
                   240 - DOT_PX / 2 + (int)st->dy);
}

static void tilt_init(scene_t *s, lv_obj_t *parent)
{
    tilt_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;

    /* Roman numeral. */
    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(st->roman, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_50, 0);
    lv_obj_set_style_text_letter_space(st->roman, 6, 0);
    lv_label_set_text(st->roman, "IV");
    lv_obj_align(st->roman, LV_ALIGN_TOP_MID, 0, 100);

    /* Reference ring — static frame of reference. */
    st->ref = lv_obj_create(parent);
    lv_obj_remove_style_all(st->ref);
    lv_obj_set_size(st->ref, REF_RING_R * 2, REF_RING_R * 2);
    lv_obj_center(st->ref);
    lv_obj_set_style_bg_opa(st->ref, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(st->ref, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(st->ref, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_border_width(st->ref, 1, 0);
    lv_obj_set_style_border_opa(st->ref, LV_OPA_30, 0);
    lv_obj_clear_flag(st->ref, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(st->ref, LV_OBJ_FLAG_CLICKABLE);

    /* The bubble. */
    st->dot = lv_obj_create(parent);
    lv_obj_remove_style_all(st->dot);
    lv_obj_set_size(st->dot, DOT_PX, DOT_PX);
    lv_obj_set_pos(st->dot, 240 - DOT_PX / 2, 240 - DOT_PX / 2);
    lv_obj_set_style_radius(st->dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(st->dot, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_bg_opa(st->dot, LV_OPA_90, 0);
    lv_obj_set_style_border_width(st->dot, 0, 0);
    lv_obj_clear_flag(st->dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(st->dot, LV_OBJ_FLAG_CLICKABLE);

    /* 20 Hz position update. Start paused — resume on first on_show. */
    st->timer = lv_timer_create(tilt_tick, 50, st);
    lv_timer_pause(st->timer);
}

static void tilt_on_show(scene_t *s)
{
    tilt_state_t *st = (tilt_state_t *)s->user_data;
    if (st && st->timer) lv_timer_resume(st->timer);
}
static void tilt_on_hide(scene_t *s)
{
    tilt_state_t *st = (tilt_state_t *)s->user_data;
    if (st && st->timer) lv_timer_pause(st->timer);
}

scene_t scene_tilt = {
    .id           = "tilt",
    .display_name = "IV. Tilt",
    .accent       = LV_COLOR_MAKE(0xB0, 0x84, 0xEE),
    .description  = "Gravity ball follows IMU accel readings, live",
    .tags         = "imu,accel,live",
    .init         = tilt_init,
    .on_show      = tilt_on_show,
    .on_hide      = tilt_on_hide,
};
