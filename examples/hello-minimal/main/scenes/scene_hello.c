/*
 * scene_hello.c — minimal scene template. Centered label, no animation,
 * no peripheral reads. Copy to scene_<your>.c, edit, register in
 * hello_minimal_main.c.
 */

#include "harness/scene_framework.h"
#include "lvgl.h"

static void hello_init(scene_t *s, lv_obj_t *parent)
{
    (void)s;
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x8FD9FF), 0);
    lv_label_set_text(label, "Hello, aurora-harness");
    lv_obj_center(label);
}

scene_t scene_hello = {
    .id           = "hello",
    .display_name = "I. Hello",
    .accent       = LV_COLOR_MAKE(0x8F, 0xD9, 0xFF),
    .description  = "Default starter scene; centered label.",
    .tags         = "demo,starter",
    .init         = hello_init,
};
