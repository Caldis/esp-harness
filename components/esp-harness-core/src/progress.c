/*
 * progress.c — long-running task overlay. See progress.h.
 */

#include "harness/progress.h"

#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

#define BAR_W   300
#define BAR_H   8
#define BG_PAD  18
#define TEXT_MAX 96

typedef struct {
    char text[TEXT_MAX];
    int  percent;
} progress_req_t;

static lv_obj_t *s_panel = NULL;
static lv_obj_t *s_label = NULL;
static lv_obj_t *s_bar   = NULL;

static void ensure_widgets(void)
{
    if (s_panel) return;

    /* Background panel — translucent black, sits above any scene. */
    s_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, BAR_W + 2 * BG_PAD, 70);
    lv_obj_set_style_bg_color(s_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_panel, 10, 0);
    lv_obj_set_style_border_width(s_panel, 1, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_panel, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(s_panel, BG_PAD, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_panel, LV_ALIGN_BOTTOM_MID, 0, -40);

    /* Label */
    s_label = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_label, lv_color_white(), 0);
    lv_obj_align(s_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(s_label, "");

    /* Bar */
    s_bar = lv_bar_create(s_panel);
    lv_obj_set_size(s_bar, BAR_W, BAR_H);
    lv_obj_align(s_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x404040), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 2, 0);
    lv_obj_set_style_radius(s_bar, 2, LV_PART_INDICATOR);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
}

static void purge(void)
{
    if (s_panel) {
        lv_obj_delete(s_panel);
        s_panel = NULL;
        s_label = NULL;
        s_bar   = NULL;
    }
}

static void show_async(void *arg)
{
    progress_req_t *req = (progress_req_t *)arg;
    if (!req) return;
    ensure_widgets();
    lv_label_set_text(s_label, req->text);
    int p = req->percent;
    if (p < 0)   p = 0;
    if (p > 100) p = 100;
    lv_bar_set_value(s_bar, p, LV_ANIM_OFF);
    free(req);
}

static void dismiss_async(void *arg)
{
    (void)arg;
    purge();
}

void harness_progress_show(const char *text, int percent)
{
    progress_req_t *req = (progress_req_t *)malloc(sizeof(*req));
    if (!req) return;
    if (text) {
        strncpy(req->text, text, sizeof(req->text) - 1);
        req->text[sizeof(req->text) - 1] = '\0';
    } else {
        req->text[0] = '\0';
    }
    req->percent = percent;
    lv_async_call(show_async, req);
}

void harness_progress_dismiss(void)
{
    lv_async_call(dismiss_async, NULL);
}
