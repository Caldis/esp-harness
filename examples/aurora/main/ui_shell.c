/*
 * ui_shell implementation.
 *
 * Tasteful, restrained, AMOLED-optimised:
 *  · pure black background everywhere (already the screen default)
 *  · hairline 1-px strokes
 *  · tiny typography (12 px) where info isn't critical
 *  · low opacity on chrome — present without shouting
 */

#include "ui_shell.h"
#include "lvgl.h"
#include "esp_log.h"

#define TAG "ui_shell"

#define DOT_R_ACTIVE   4
#define DOT_R_INACTIVE 2
#define DOT_SPACING    14

static lv_obj_t *s_label = NULL;
static lv_obj_t *s_fps   = NULL;
static lv_obj_t *s_dots_row = NULL;
static lv_obj_t **s_dots = NULL;
static int       s_dot_count = 0;
static int       s_active_idx = -1;
static bool      s_chrome_visible = true;

void ui_shell_init(int scene_count)
{
    lv_obj_t *layer = lv_layer_top();
    lv_obj_remove_style_all(layer);
    lv_obj_set_size(layer, LV_PCT(100), LV_PCT(100));

    /* ── Scene label (top arc) ───────────────────────────────────────── */
    s_label = lv_label_create(layer);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_label, lv_color_hex(0xEEEEEE), 0);
    lv_obj_set_style_text_opa(s_label, LV_OPA_70, 0);
    lv_obj_set_style_text_letter_space(s_label, 1, 0);
    lv_label_set_text(s_label, "");
    /* The screen is round; place the label well below the top edge to
     * dodge the curvature. */
    lv_obj_align(s_label, LV_ALIGN_TOP_MID, 0, 60);

    /* ── FPS read-out (top-right, tiny) ──────────────────────────────── */
    s_fps = lv_label_create(layer);
    lv_obj_set_style_text_font(s_fps, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_fps, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_opa(s_fps, LV_OPA_30, 0);
    lv_obj_set_style_text_letter_space(s_fps, 2, 0);
    lv_label_set_text(s_fps, "--");
    lv_obj_align(s_fps, LV_ALIGN_TOP_RIGHT, -70, 80);  /* inset from round edge */

    /* ── Indicator dots row (bottom) ─────────────────────────────────── */
    s_dot_count = scene_count;
    s_dots = lv_malloc(sizeof(lv_obj_t *) * (size_t)scene_count);

    s_dots_row = lv_obj_create(layer);
    lv_obj_remove_style_all(s_dots_row);
    int total_w = scene_count * (DOT_R_ACTIVE * 2) + (scene_count - 1) * DOT_SPACING;
    lv_obj_set_size(s_dots_row, total_w + 10, DOT_R_ACTIVE * 2 + 8);
    lv_obj_set_flex_flow(s_dots_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dots_row,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_dots_row, DOT_SPACING - DOT_R_ACTIVE * 2, 0);
    /* Place inside the round edge — about 60 px from bottom. */
    lv_obj_align(s_dots_row, LV_ALIGN_BOTTOM_MID, 0, -60);

    for (int i = 0; i < scene_count; ++i) {
        lv_obj_t *d = lv_obj_create(s_dots_row);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, DOT_R_INACTIVE * 2, DOT_R_INACTIVE * 2);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_30, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        s_dots[i] = d;
    }

    /* Critically: the top layer must NOT eat touch events meant for scenes.
     * The label / fps / dots are decorations only; mark them as event-pass. */
    lv_obj_add_flag(s_label,    LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(s_fps,      LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(s_dots_row, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(s_label,    LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_fps,      LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_dots_row, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < scene_count; ++i) {
        lv_obj_clear_flag(s_dots[i], LV_OBJ_FLAG_CLICKABLE);
    }

    /* Top layer is always above screens; ensure it's transparent so scenes
     * show through. */
    lv_obj_set_style_bg_opa(layer, LV_OPA_TRANSP, 0);

    ESP_LOGI(TAG, "shell mounted with %d dots", scene_count);
}

void ui_shell_set_active(int idx, const char *display_name)
{
    if (idx < 0 || idx >= s_dot_count) return;
    s_active_idx = idx;
    if (s_label && display_name) {
        lv_label_set_text(s_label, display_name);
    }
    for (int i = 0; i < s_dot_count; ++i) {
        bool active = (i == idx);
        lv_obj_set_size(s_dots[i],
                        (active ? DOT_R_ACTIVE : DOT_R_INACTIVE) * 2,
                        (active ? DOT_R_ACTIVE : DOT_R_INACTIVE) * 2);
        lv_obj_set_style_bg_opa(s_dots[i], active ? LV_OPA_90 : LV_OPA_30, 0);
    }
}

void ui_shell_set_fps(float fps)
{
    if (!s_fps) return;
    if (fps <= 0.0f) {
        lv_label_set_text(s_fps, "--");
    } else {
        lv_label_set_text_fmt(s_fps, "%.0f", fps);
    }
}

void ui_shell_toggle_chrome(void)
{
    s_chrome_visible = !s_chrome_visible;
    lv_opa_t op = s_chrome_visible ? LV_OPA_COVER : LV_OPA_TRANSP;
    if (s_label)    lv_obj_set_style_opa(s_label, op, 0);
    if (s_fps)      lv_obj_set_style_opa(s_fps, op, 0);
    if (s_dots_row) lv_obj_set_style_opa(s_dots_row, op, 0);
}
