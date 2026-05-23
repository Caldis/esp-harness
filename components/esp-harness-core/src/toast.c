/*
 * toast.c — overlay notification primitive. See toast.h for the contract.
 *
 * Implementation: every harness_toast() call mallocs a request, posts it
 * to LVGL via lv_async_call. The dispatched handler nukes any existing
 * toast widget + timer, then creates a fresh label on lv_layer_top()
 * and schedules a one-shot lv_timer to remove it. lv_layer_top() sits
 * above every scene's container so the toast is visible regardless of
 * which scene is current.
 */

#include "harness/toast.h"

#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

#define TOAST_DEFAULT_MS  1500
#define TOAST_MAX_TEXT    96

typedef struct {
    char     text[TOAST_MAX_TEXT];
    uint32_t duration_ms;
} toast_req_t;

static lv_obj_t   *s_label = NULL;
static lv_timer_t *s_timer = NULL;

static void toast_purge(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_label) {
        lv_obj_delete(s_label);
        s_label = NULL;
    }
}

static void toast_expire_cb(lv_timer_t *t)
{
    (void)t;
    toast_purge();
}

static void toast_show_async(void *arg)
{
    toast_req_t *req = (toast_req_t *)arg;
    if (!req) return;

    /* Replace any prior in-flight toast. */
    toast_purge();

    lv_obj_t *t = lv_label_create(lv_layer_top());
    lv_label_set_text(t, req->text);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_set_style_bg_color(t, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(t, 12, 0);
    lv_obj_set_style_radius(t, 10, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, lv_color_white(), 0);
    lv_obj_set_style_border_opa(t, LV_OPA_30, 0);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -40);
    s_label = t;

    s_timer = lv_timer_create(toast_expire_cb, req->duration_ms, NULL);
    lv_timer_set_repeat_count(s_timer, 1);

    free(req);
}

static void toast_dismiss_async(void *arg)
{
    (void)arg;
    toast_purge();
}

void harness_toast(const char *text, uint32_t duration_ms)
{
    if (!text) return;
    toast_req_t *req = (toast_req_t *)malloc(sizeof(*req));
    if (!req) return;
    strncpy(req->text, text, sizeof(req->text) - 1);
    req->text[sizeof(req->text) - 1] = '\0';
    req->duration_ms = duration_ms ? duration_ms : TOAST_DEFAULT_MS;
    lv_async_call(toast_show_async, req);
}

void harness_toast_dismiss(void)
{
    lv_async_call(toast_dismiss_async, NULL);
}
