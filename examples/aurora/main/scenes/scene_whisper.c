/*
 * Scene VIII · Whisper — BLE observer, visualised.
 *
 * Long-press kicks off a 2-second BLE scan. Each unique device the
 * radio sees during the window appears as a small dot, placed on a
 * polar canvas:
 *
 *   - **angle** is derived from a hash of the MAC address (stable for
 *     a given device, so the same earbud sits in the same direction
 *     across scans),
 *   - **radius** is inversely proportional to RSSI (closer = stronger
 *     signal = smaller radius), clamped to [40, 200] px.
 *
 * A central counter shows the raw `adv_events` for the scan — the AI's
 * smoking-gun proof: a stalled stack returns 0 advs even if it
 * accumulates a stale device list.
 *
 * Aesthetic: cool muted indigo; one accent. Hairline outer ring as the
 * "world".
 */

#include "harness/scene_framework.h"
#include "lvgl.h"
#include "peripherals/ble.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define ACCENT       0x8A8AD8     /* muted indigo */
#define OUTER_R      210
#define DOT_PX       8
#define MAX_DOTS     BLE_SCAN_MAX_DEVICES

typedef struct {
    lv_obj_t *roman;
    lv_obj_t *outer;
    lv_obj_t *centre_count;
    lv_obj_t *adv_count;
    lv_obj_t *hint;
    lv_obj_t *dots[MAX_DOTS];
    int       dot_count;
    bool      scanning;
} whisper_state_t;

static int hash_addr(const uint8_t addr[6])
{
    uint32_t h = 0x811C9DC5u;
    for (int i = 0; i < 6; ++i) {
        h ^= addr[i];
        h *= 0x01000193u;
    }
    return (int)(h % 360);
}

static void clear_dots(whisper_state_t *st)
{
    for (int i = 0; i < st->dot_count; ++i) {
        if (st->dots[i]) {
            lv_obj_del(st->dots[i]);
            st->dots[i] = NULL;
        }
    }
    st->dot_count = 0;
}

static lv_obj_t *make_dot(lv_obj_t *parent)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, DOT_PX, DOT_PX);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_90, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
    return d;
}

/* The scan runs on its own task — ble_scan blocks for ~3s, which is
 * far too long for the LVGL task. We hand it off and apply the results
 * back on the LVGL task via lv_async_call (LVGL's thread-safe ferry). */
typedef struct {
    whisper_state_t *st;
    ble_device_t devices[MAX_DOTS];
    int           dev_count;
    int           adv_events;
    int           elapsed_ms;
} scan_result_t;

static void apply_scan_lvgl(void *arg)
{
    scan_result_t *r = (scan_result_t *)arg;
    if (!r || !r->st) { lv_free(r); return; }
    whisper_state_t *st = r->st;

    clear_dots(st);
    for (int i = 0; i < r->dev_count && i < MAX_DOTS; ++i) {
        const ble_device_t *d = &r->devices[i];
        /* RSSI clamp: -30 (close) → r ~50; -100 (far) → r ~200. */
        int rssi = (int)d->rssi;
        if (rssi > -30)  rssi = -30;
        if (rssi < -100) rssi = -100;
        float t = (float)(-rssi - 30) / 70.0f;   /* 0..1 */
        int radius = 50 + (int)(t * 150.0f);     /* 50..200 */

        float angle = (float)hash_addr(d->addr) * (float)M_PI / 180.0f;
        int x = 240 + (int)(cosf(angle) * radius) - DOT_PX / 2;
        int y = 240 + (int)(sinf(angle) * radius) - DOT_PX / 2;

        lv_obj_t *dot = make_dot(lv_obj_get_parent(st->outer));
        lv_obj_set_pos(dot, x, y);
        st->dots[i] = dot;
    }
    st->dot_count = r->dev_count;

    char buf[40];
    snprintf(buf, sizeof(buf), "%d  dev",  r->dev_count);
    lv_label_set_text(st->centre_count, buf);
    snprintf(buf, sizeof(buf), "%d  adv  in  %d ms", r->adv_events, r->elapsed_ms);
    lv_label_set_text(st->adv_count, buf);
    lv_label_set_text(st->hint, "hold to rescan");
    st->scanning = false;
    lv_free(r);
}

static void scan_task(void *arg)
{
    scan_result_t *r = (scan_result_t *)arg;
    int64_t t0 = esp_timer_get_time();
    r->dev_count = ble_scan(r->devices, MAX_DOTS, 2000, &r->adv_events);
    r->elapsed_ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (r->dev_count < 0) {
        r->dev_count  = 0;
        r->adv_events = 0;
    }
    lv_async_call(apply_scan_lvgl, r);
    vTaskDelete(NULL);
}

static void whisper_long_press(scene_t *s)
{
    whisper_state_t *st = (whisper_state_t *)s->user_data;
    if (!st || st->scanning) return;
    st->scanning = true;
    lv_label_set_text(st->hint, "scanning...");

    scan_result_t *r = (scan_result_t *)lv_malloc_zeroed(sizeof(*r));
    if (!r) { st->scanning = false; return; }
    r->st = st;
    /* 2.5 KB internal-SRAM stack — scan task body is shallow (one
     * `ble_scan` blocking call) and the result struct lives in PSRAM
     * via lv_malloc_zeroed. Larger stacks fail to allocate when BLE,
     * audio, and LVGL together have already squeezed internal SRAM. */
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    BaseType_t ok = xTaskCreate(scan_task, "ble_scan", 2560, r, 5, NULL);
    if (ok != pdPASS) {
        char b[40];
        snprintf(b, sizeof(b), "fail int=%uK lg=%uK",
                 (unsigned)(free_int / 1024), (unsigned)(largest / 1024));
        lv_label_set_text(st->hint, b);
        lv_free(r);
        st->scanning = false;
    }
}

static void whisper_init(scene_t *s, lv_obj_t *parent)
{
    whisper_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;

    /* Roman VIII */
    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(st->roman, 6, 0);
    lv_obj_set_style_text_color(st->roman, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_50, 0);
    lv_label_set_text(st->roman, "VIII");
    lv_obj_align(st->roman, LV_ALIGN_TOP_MID, 0, 100);

    /* Outer ring — the "world" of observed space. */
    st->outer = lv_obj_create(parent);
    lv_obj_remove_style_all(st->outer);
    lv_obj_set_size(st->outer, OUTER_R * 2, OUTER_R * 2);
    lv_obj_center(st->outer);
    lv_obj_set_style_bg_opa(st->outer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(st->outer, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(st->outer, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_border_width(st->outer, 1, 0);
    lv_obj_set_style_border_opa(st->outer, LV_OPA_20, 0);
    lv_obj_clear_flag(st->outer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(st->outer, LV_OBJ_FLAG_SCROLLABLE);

    /* Centre device count + adv events + hint */
    st->centre_count = lv_label_create(parent);
    lv_obj_set_style_text_font(st->centre_count, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(st->centre_count, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->centre_count, LV_OPA_90, 0);
    lv_label_set_text(st->centre_count, "-");
    lv_obj_align(st->centre_count, LV_ALIGN_CENTER, 0, -8);

    st->adv_count = lv_label_create(parent);
    lv_obj_set_style_text_font(st->adv_count, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->adv_count, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->adv_count, LV_OPA_50, 0);
    lv_label_set_text(st->adv_count, "");
    lv_obj_align(st->adv_count, LV_ALIGN_CENTER, 0, 22);

    st->hint = lv_label_create(parent);
    lv_obj_set_style_text_font(st->hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->hint, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->hint, LV_OPA_50, 0);
    lv_label_set_text(st->hint, "hold to scan");
    lv_obj_align(st->hint, LV_ALIGN_CENTER, 0, 48);
}

scene_t scene_whisper = {
    .id           = "whisper",
    .display_name = "VIII. Whisper",
    .accent       = LV_COLOR_MAKE(0x8A, 0x8A, 0xD8),
    .init         = whisper_init,
    .on_long_press = whisper_long_press,
};
