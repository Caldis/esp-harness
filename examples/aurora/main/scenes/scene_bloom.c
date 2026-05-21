/*
 * Scene III · Bloom — radial seed dispersal.
 *
 * Small bright dots placed along a Fibonacci-like spiral, packed densely
 * near the centre and sparsely toward the rim. Phyllotaxis pattern (the
 * one you see in sunflower seed heads). Soft rose on AMOLED black.
 *
 * Static — the visual is a snapshot of a perfect arrangement; motion
 * would only spoil it.
 */

#include "harness/scene_framework.h"
#include "lvgl.h"
#include <math.h>

#define ACCENT  0xE89BB5    /* soft rose */
#define CENTRE_X 233
#define CENTRE_Y 233
#define N_DOTS   144        /* a Fibonacci number — feels right */
#define R_MAX    210
#define GOLDEN_ANGLE (137.50776405f * (float)M_PI / 180.0f)

static void bloom_init(scene_t *s, lv_obj_t *parent)
{
    (void)s;

    /* Roman numeral. */
    lv_obj_t *roman = lv_label_create(parent);
    lv_obj_set_style_text_font(roman, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(roman, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(roman, LV_OPA_50, 0);
    lv_obj_set_style_text_letter_space(roman, 6, 0);
    lv_label_set_text(roman, "III");
    lv_obj_align(roman, LV_ALIGN_TOP_MID, 0, 100);

    /* Phyllotaxis: each seed at angle = i * golden_angle, radius = sqrt(i). */
    for (int i = 0; i < N_DOTS; ++i) {
        float t = (float)(i + 1);
        float a = (float)i * GOLDEN_ANGLE;
        float r = R_MAX * sqrtf(t / (float)N_DOTS);

        int x = CENTRE_X + (int)(r * cosf(a));
        int y = CENTRE_Y + (int)(r * sinf(a));

        /* Dot size grows slightly with distance from centre (denser inside
         * looks tidier when dots are tiny; outer dots can afford to be a
         * touch larger). Opacity slightly fades outward. */
        int sz = 3 + (int)(2.0f * r / R_MAX);   /* 3..5 px */
        lv_opa_t opa = (lv_opa_t)(LV_OPA_70 - (int)(50.0f * r / R_MAX));

        lv_obj_t *d = lv_obj_create(parent);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, sz, sz);
        lv_obj_set_pos(d, x - sz / 2, y - sz / 2);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(ACCENT), 0);
        lv_obj_set_style_bg_opa(d, opa, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
    }
}

scene_t scene_bloom = {
    .id           = "bloom",
    .display_name = "III. Bloom",
    .accent       = LV_COLOR_MAKE(0xE8, 0x9B, 0xB5),
    .init         = bloom_init,
};
