/*
 * Scene II · Grid — radial compass rose.
 *
 * Twelve hairline strokes radiating from the centre to the bezel edge,
 * like a clock face or a wind rose. Two of them are accented (a longer,
 * brighter "12 o'clock" and "6 o'clock") to give the composition an axis.
 *
 * Warm amber accent on AMOLED black. Static.
 */

#include "harness/scene_framework.h"
#include "lvgl.h"
#include <math.h>

#define ACCENT_VAL 0xE0B25C  /* warm amber */

#define N_SPOKES 12
#define R_OUTER  220
#define R_INNER  70
#define CENTRE_X 233
#define CENTRE_Y 233

static void grid_init(scene_t *s, lv_obj_t *parent)
{
    (void)s;

    /* Roman numeral. */
    lv_obj_t *roman = lv_label_create(parent);
    lv_obj_set_style_text_font(roman, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(roman, lv_color_hex(ACCENT_VAL), 0);
    lv_obj_set_style_text_opa(roman, LV_OPA_50, 0);
    lv_obj_set_style_text_letter_space(roman, 6, 0);
    lv_label_set_text(roman, "II");
    lv_obj_align(roman, LV_ALIGN_TOP_MID, 0, 100);

    /* Radial spokes using lv_line. */
    static lv_point_precise_t pts[N_SPOKES][2];
    for (int i = 0; i < N_SPOKES; ++i) {
        float angle = (float)i * (2.0f * (float)M_PI / N_SPOKES) - (float)M_PI_2;
        float r_in = R_INNER;
        float r_out = R_OUTER;
        /* Highlight 12 o'clock + 6 o'clock — make them extend further inward. */
        if (i == 0 || i == N_SPOKES / 2) {
            r_in = R_INNER - 26;
        }
        pts[i][0].x = (lv_value_precise_t)(CENTRE_X + r_in * cosf(angle));
        pts[i][0].y = (lv_value_precise_t)(CENTRE_Y + r_in * sinf(angle));
        pts[i][1].x = (lv_value_precise_t)(CENTRE_X + r_out * cosf(angle));
        pts[i][1].y = (lv_value_precise_t)(CENTRE_Y + r_out * sinf(angle));

        lv_obj_t *ln = lv_line_create(parent);
        lv_line_set_points(ln, pts[i], 2);
        /* Axis spokes are brighter / thicker. */
        bool axis = (i == 0 || i == N_SPOKES / 2);
        lv_obj_set_style_line_color(ln, lv_color_hex(ACCENT_VAL), 0);
        lv_obj_set_style_line_width(ln, axis ? 2 : 1, 0);
        lv_obj_set_style_line_opa(ln, axis ? LV_OPA_90 : LV_OPA_50, 0);
    }

    /* A single inner ring binds the composition. */
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, R_INNER * 2, R_INNER * 2);
    lv_obj_center(ring);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(ACCENT_VAL), 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_70, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
}

scene_t scene_grid = {
    .id           = "grid",
    .display_name = "II. Grid",
    .accent       = LV_COLOR_MAKE(0xE0, 0xB2, 0x5C),
    .description  = "Twelve hairline radials (compass rose), warm amber, static",
    .tags         = "display,static,art",
    .init         = grid_init,
};
