/*
 * Scene IX · Spectrum — WiFi peripheral, visualised.
 *
 * Long-press triggers a WiFi STA scan and lays out the discovered APs
 * around the screen as radial dots:
 *
 *   - **angle** is derived from the AP's channel (1..14 → 0..360°),
 *     so 2.4 GHz channel layout becomes a clock face.
 *   - **radius** is inversely proportional to RSSI (closer/stronger
 *     APs sit nearer the centre).
 *
 * **Coexistence:** by default Aurora boots with BLE eagerly initialised
 * and BLE's pool reservation leaves too little internal SRAM for WiFi
 * to start. The first long-press in Spectrum therefore performs a
 * `ble_deinit` to free that SRAM, then brings WiFi up. After that,
 * `ble scan` returns -1 until reboot — the trade-off is explicit and
 * surfaced in the on-screen state line.
 *
 * Aesthetic: cool silver-blue accent.
 */

#include "harness/scene_framework.h"
#include "lvgl.h"
#include "peripherals/wifi.h"
#include "peripherals/ble.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define ACCENT       0xA9C8DC     /* silver blue */
#define OUTER_R      210
#define DOT_PX       9
#define MAX_DOTS     WIFI_SCAN_MAX_RESULTS

typedef struct {
    lv_obj_t *roman;
    lv_obj_t *outer;
    lv_obj_t *count_label;
    lv_obj_t *top_label;
    lv_obj_t *state_label;
    lv_obj_t *dots[MAX_DOTS];
    int       dot_count;
    bool      scanning;
} spectrum_state_t;

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

static void clear_dots(spectrum_state_t *st)
{
    for (int i = 0; i < st->dot_count; ++i) {
        if (st->dots[i]) {
            lv_obj_del(st->dots[i]);
            st->dots[i] = NULL;
        }
    }
    st->dot_count = 0;
}

typedef struct {
    spectrum_state_t *st;
    wifi_ap_t aps[MAX_DOTS];
    int       ap_count;
    int       elapsed_ms;
    bool      ble_was_up;
} scan_result_t;

static void apply_scan_lvgl(void *arg)
{
    scan_result_t *r = (scan_result_t *)arg;
    if (!r || !r->st) { lv_free(r); return; }
    spectrum_state_t *st = r->st;

    clear_dots(st);
    char top_buf[40] = "-";
    if (r->ap_count > 0) {
        snprintf(top_buf, sizeof(top_buf), "top: %.20s  %d dBm",
                 r->aps[0].ssid, (int)r->aps[0].rssi);
    }

    for (int i = 0; i < r->ap_count && i < MAX_DOTS; ++i) {
        const wifi_ap_t *a = &r->aps[i];
        int ch = a->channel;
        if (ch < 1)  ch = 1;
        if (ch > 14) ch = 14;
        float angle = ((float)(ch - 1) / 14.0f) * 2.0f * (float)M_PI - (float)M_PI_2;

        int rssi = (int)a->rssi;
        if (rssi > -30)  rssi = -30;
        if (rssi < -100) rssi = -100;
        float t = (float)(-rssi - 30) / 70.0f;
        int radius = 50 + (int)(t * 150.0f);

        int x = 240 + (int)(cosf(angle) * radius) - DOT_PX / 2;
        int y = 240 + (int)(sinf(angle) * radius) - DOT_PX / 2;
        lv_obj_t *dot = make_dot(lv_obj_get_parent(st->outer));
        lv_obj_set_pos(dot, x, y);
        st->dots[i] = dot;
    }
    st->dot_count = r->ap_count;

    char buf[40];
    snprintf(buf, sizeof(buf), "%d  AP", r->ap_count);
    lv_label_set_text(st->count_label, buf);
    lv_label_set_text(st->top_label, top_buf);
    snprintf(buf, sizeof(buf), "scan %d ms%s",
             r->elapsed_ms, r->ble_was_up ? "  (ble released)" : "");
    lv_label_set_text(st->state_label, buf);

    st->scanning = false;
    lv_free(r);
}

static void scan_task(void *arg)
{
    scan_result_t *r = (scan_result_t *)arg;
    int64_t t0 = esp_timer_get_time();
    r->ap_count = wifi_scan(r->aps, MAX_DOTS, 600);
    r->elapsed_ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (r->ap_count < 0) r->ap_count = 0;
    lv_async_call(apply_scan_lvgl, r);
    vTaskDelete(NULL);
}

static void spectrum_long_press(scene_t *s)
{
    spectrum_state_t *st = (spectrum_state_t *)s->user_data;
    if (!st || st->scanning) return;
    st->scanning = true;

    scan_result_t *r = (scan_result_t *)lv_malloc_zeroed(sizeof(*r));
    if (!r) { st->scanning = false; return; }
    r->st = st;

    /* CRITICAL ORDER: free the BLE controller's internal-SRAM pool
     * FIRST, before the wifi task is created. If we tried xTaskCreate
     * with BLE still up, the alloc fails — same internal-SRAM
     * starvation that bit `audio_play_tone_async`'s old 4 KB stack. */
    r->ble_was_up = ble_is_up();
    if (r->ble_was_up) {
        lv_label_set_text(st->state_label, "releasing ble...");
        ble_deinit();
        /* Give the controller 80 ms to actually wind down before
         * WiFi tries to claim the radio. */
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    lv_label_set_text(st->state_label, "scanning...");

    BaseType_t ok = xTaskCreate(scan_task, "wifi_scan", 4096, r, 5, NULL);
    if (ok != pdPASS) {
        lv_free(r);
        st->scanning = false;
        char b[60];
        snprintf(b, sizeof(b), "fail int=%uK lg=%uK",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
        lv_label_set_text(st->state_label, b);
    }
}

static void spectrum_init(scene_t *s, lv_obj_t *parent)
{
    spectrum_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;

    /* Roman IX */
    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(st->roman, 6, 0);
    lv_obj_set_style_text_color(st->roman, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_50, 0);
    lv_label_set_text(st->roman, "IX");
    lv_obj_align(st->roman, LV_ALIGN_TOP_MID, 0, 100);

    /* Outer ring — channel face */
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

    /* Count + top + state */
    st->count_label = lv_label_create(parent);
    lv_obj_set_style_text_font(st->count_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(st->count_label, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->count_label, LV_OPA_90, 0);
    lv_label_set_text(st->count_label, "-");
    lv_obj_align(st->count_label, LV_ALIGN_CENTER, 0, -10);

    st->top_label = lv_label_create(parent);
    lv_obj_set_style_text_font(st->top_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->top_label, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->top_label, LV_OPA_60, 0);
    lv_label_set_text(st->top_label, "hold to scan");
    lv_obj_align(st->top_label, LV_ALIGN_CENTER, 0, 18);

    st->state_label = lv_label_create(parent);
    lv_obj_set_style_text_font(st->state_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->state_label, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->state_label, LV_OPA_50, 0);
    lv_label_set_text(st->state_label, "");
    lv_obj_align(st->state_label, LV_ALIGN_CENTER, 0, 42);
}

scene_t scene_spectrum = {
    .id           = "spectrum",
    .display_name = "IX. Spectrum",
    .accent       = LV_COLOR_MAKE(0xA9, 0xC8, 0xDC),
    .init         = spectrum_init,
    .on_long_press = spectrum_long_press,
};
