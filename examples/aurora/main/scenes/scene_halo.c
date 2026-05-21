/*
 * Scene I · Halo — concentric rings, contemplative.
 *
 * Three stationary rings of decreasing opacity outward, a small bright
 * centre disc, an ice-blue accent. Pure black background.
 *
 * Static composition — no animation. Widget-based (lv_obj per ring).
 *
 * Tried canvas-based rendering for multi-scene support (see git log).
 * Result: a single 466×466 RGB565 canvas widget triggers exactly the
 * same CO5300 QSPI DMA TX underflow as N widgets do, because the
 * limiting factor is the bus's ability to flush a full-screen region
 * in one burst, not the widget count. Fix needs deeper BSP work
 * (flush chunking / DMA buffer sizing) — tracked in KNOWN_ISSUES §1.
 */

#include "harness/scene_framework.h"
#include "lvgl.h"

#define ACCENT  0x8FD9FF       /* ice blue */

static void halo_init(scene_t *s, lv_obj_t *parent)
{
    (void)s;

    lv_obj_t *roman = lv_label_create(parent);
    lv_obj_set_style_text_font(roman, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(roman, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(roman, LV_OPA_50, 0);
    lv_obj_set_style_text_letter_space(roman, 6, 0);
    lv_label_set_text(roman, "I");
    lv_obj_align(roman, LV_ALIGN_TOP_MID, 0, 100);

    int radii[]   = {  78, 144, 215 };
    int opas[]    = { 230, 130,  55 };
    int widths[]  = {   2,   2,   1 };
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *r = lv_obj_create(parent);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, radii[i] * 2, radii[i] * 2);
        lv_obj_center(r);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(ACCENT), 0);
        lv_obj_set_style_border_width(r, widths[i], 0);
        lv_obj_set_style_border_opa(r, opas[i], 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *centre = lv_obj_create(parent);
    lv_obj_remove_style_all(centre);
    lv_obj_set_size(centre, 14, 14);
    lv_obj_center(centre);
    lv_obj_set_style_radius(centre, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(centre, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_bg_opa(centre, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(centre, 0, 0);
    lv_obj_clear_flag(centre, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(centre, LV_OBJ_FLAG_CLICKABLE);
}

scene_t scene_halo = {
    .id           = "halo",
    .display_name = "I. Halo",
    .accent       = LV_COLOR_MAKE(0x8F, 0xD9, 0xFF),
    .description  = "Three concentric rings with a centre disc, no animation",
    .tags         = "display,static,art",
    .init         = halo_init,
};
