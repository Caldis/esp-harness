/*
 * Scene XVIII · Sniff — BLE devices, tabular.
 *
 * Complement to Scene VIII (Whisper). Whisper is art (RSSI -> radial
 * dot, adv_event tally); Sniff is data (top-8 list with name-or-addr,
 * RSSI bar, addr type). Same backend (peripherals/ble.c), two
 * presentations.
 *
 * Controls:
 *   short tap: next scene
 *   long press: trigger 2 s passive scan
 *   BOOT key edge: same as long press
 *   USER key edge: toggle display mode (name / addr)
 *
 * Coexistence: requires BLE to be up. If WiFi has been active, BLE
 * cannot return without reboot — Sniff will show that state.
 */

#include "harness/scene_framework.h"
#include "lvgl.h"
#include "peripherals/ble.h"
#include "peripherals/keys.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define ACCENT       0xE8C387     /* warm amber */
#define ROW_COUNT    8
#define ROW_H        24
#define BAR_W        70
#define SCAN_MS      2000

typedef enum {
    MODE_NAME = 0,
    MODE_ADDR = 1,
} sniff_mode_t;

typedef struct {
    lv_obj_t *roman;
    lv_obj_t *header;
    lv_obj_t *state_label;
    lv_obj_t *mode_label;
    lv_obj_t *row_left[ROW_COUNT];
    lv_obj_t *row_right[ROW_COUNT];
    lv_obj_t *row_bar[ROW_COUNT];

    ble_device_t devices[BLE_SCAN_MAX_DEVICES];
    int          dev_count;
    int          adv_events;
    int          elapsed_ms;
    bool         scanning;
    sniff_mode_t mode;

    int          last_boot_count;
    int          last_user_count;
} sniff_state_t;

static int cmp_rssi_desc(const void *a, const void *b)
{
    return ((const ble_device_t *)b)->rssi - ((const ble_device_t *)a)->rssi;
}

static void render_rows(sniff_state_t *st)
{
    if (st->dev_count > 1) {
        qsort(st->devices, st->dev_count, sizeof(ble_device_t), cmp_rssi_desc);
    }
    for (int i = 0; i < ROW_COUNT; ++i) {
        if (i < st->dev_count) {
            const ble_device_t *d = &st->devices[i];
            char left[22];
            if (st->mode == MODE_NAME && d->name[0]) {
                snprintf(left, sizeof(left), "%.20s", d->name);
            } else {
                snprintf(left, sizeof(left), "%02x:%02x:%02x:%02x:%02x:%02x",
                         d->addr[0], d->addr[1], d->addr[2],
                         d->addr[3], d->addr[4], d->addr[5]);
            }
            lv_label_set_text(st->row_left[i], left);

            char right[20];
            snprintf(right, sizeof(right), "%4d  %s",
                     (int)d->rssi,
                     d->addr_type == 0 ? "pub" : "rnd");
            lv_label_set_text(st->row_right[i], right);

            int rssi = (int)d->rssi;
            if (rssi > -30)  rssi = -30;
            if (rssi < -100) rssi = -100;
            int w = (BAR_W * (-30 - rssi)) / -70;
            w = BAR_W - w;
            if (w < 2) w = 2;
            lv_obj_set_width(st->row_bar[i], w);
            lv_obj_clear_flag(st->row_bar[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(st->row_left[i], "");
            lv_label_set_text(st->row_right[i], "");
            lv_obj_add_flag(st->row_bar[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    char b[40];
    snprintf(b, sizeof(b), "%d dev  %d adv  %d ms",
             st->dev_count, st->adv_events, st->elapsed_ms);
    lv_label_set_text(st->state_label, b);
    lv_label_set_text(st->mode_label,
                      st->mode == MODE_NAME ? "show: name" : "show: addr");
}

typedef struct {
    sniff_state_t *st;
    ble_device_t   devs[BLE_SCAN_MAX_DEVICES];
    int            count;
    int            adv_events;
    int            elapsed_ms;
} sniff_scan_t;

static void apply_scan(void *arg)
{
    sniff_scan_t *r = (sniff_scan_t *)arg;
    if (!r) return;
    sniff_state_t *st = r->st;
    if (!st) { lv_free(r); return; }
    memcpy(st->devices, r->devs, sizeof(st->devices));
    st->dev_count = r->count;
    st->adv_events = r->adv_events;
    st->elapsed_ms = r->elapsed_ms;
    st->scanning = false;
    render_rows(st);
    lv_free(r);
}

static void scan_task(void *arg)
{
    sniff_scan_t *r = (sniff_scan_t *)arg;
    int64_t t0 = esp_timer_get_time();
    int total_adv = 0;
    int n = ble_scan(r->devs, BLE_SCAN_MAX_DEVICES, SCAN_MS, &total_adv);
    r->elapsed_ms = (int)((esp_timer_get_time() - t0) / 1000);
    r->count = (n < 0) ? 0 : n;
    r->adv_events = total_adv;
    lv_async_call(apply_scan, r);
    vTaskDelete(NULL);
}

static void start_scan(sniff_state_t *st)
{
    if (!st || st->scanning) return;

    /* If BLE never came up yet, init it now. After WiFi has run, this
     * fails — we report it via the state label. */
    if (!ble_is_up()) {
        lv_label_set_text(st->state_label, "starting ble...");
        if (!ble_init()) {
            lv_label_set_text(st->state_label, "ble unavailable (wifi held it; reboot)");
            return;
        }
    }
    st->scanning = true;
    lv_label_set_text(st->state_label, "scanning...");
    sniff_scan_t *r = lv_malloc_zeroed(sizeof(*r));
    if (!r) { st->scanning = false; return; }
    r->st = st;
    if (xTaskCreate(scan_task, "sniff_scan", 4096, r, 5, NULL) != pdPASS) {
        lv_free(r);
        st->scanning = false;
        lv_label_set_text(st->state_label, "task create failed");
    }
}

static void sniff_long_press(scene_t *s)
{
    start_scan((sniff_state_t *)s->user_data);
}

static void sniff_frame(scene_t *s, uint32_t t_ms)
{
    (void)t_ms;
    sniff_state_t *st = (sniff_state_t *)s->user_data;
    if (!st) return;
    keys_state_t k;
    keys_get(&k);
    if ((int)k.boot_count != st->last_boot_count) {
        st->last_boot_count = (int)k.boot_count;
        if (k.boot_pressed) start_scan(st);
    }
    if ((int)k.user_count != st->last_user_count) {
        st->last_user_count = (int)k.user_count;
        if (k.user_pressed) {
            st->mode = (st->mode == MODE_NAME) ? MODE_ADDR : MODE_NAME;
            render_rows(st);
        }
    }
}

static void sniff_on_show(scene_t *s)
{
    sniff_state_t *st = (sniff_state_t *)s->user_data;
    if (!st) return;
    keys_state_t k;
    keys_get(&k);
    st->last_boot_count = (int)k.boot_count;
    st->last_user_count = (int)k.user_count;
}

static void sniff_init(scene_t *s, lv_obj_t *parent)
{
    sniff_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;
    st->mode = MODE_NAME;

    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(st->roman, 4, 0);
    lv_obj_set_style_text_color(st->roman, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_50, 0);
    lv_label_set_text(st->roman, "XVIII");
    lv_obj_align(st->roman, LV_ALIGN_TOP_MID, 0, 28);

    st->header = lv_label_create(parent);
    lv_obj_set_style_text_font(st->header, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->header, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->header, LV_OPA_60, 0);
    lv_label_set_text(st->header, "SNIFF  /  BLE devices");
    lv_obj_align(st->header, LV_ALIGN_TOP_MID, 0, 60);

    const int y0 = 100;
    for (int i = 0; i < ROW_COUNT; ++i) {
        st->row_left[i] = lv_label_create(parent);
        lv_obj_set_style_text_font(st->row_left[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(st->row_left[i], lv_color_white(), 0);
        lv_obj_set_style_text_opa(st->row_left[i], LV_OPA_90, 0);
        lv_label_set_text(st->row_left[i], "");
        lv_obj_align(st->row_left[i], LV_ALIGN_TOP_LEFT, 20, y0 + i * ROW_H);

        st->row_right[i] = lv_label_create(parent);
        lv_obj_set_style_text_font(st->row_right[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(st->row_right[i], lv_color_hex(ACCENT), 0);
        lv_obj_set_style_text_opa(st->row_right[i], LV_OPA_70, 0);
        lv_label_set_text(st->row_right[i], "");
        lv_obj_align(st->row_right[i], LV_ALIGN_TOP_RIGHT, -20, y0 + i * ROW_H + 2);

        st->row_bar[i] = lv_obj_create(parent);
        lv_obj_remove_style_all(st->row_bar[i]);
        lv_obj_set_size(st->row_bar[i], BAR_W, 2);
        lv_obj_set_style_bg_color(st->row_bar[i], lv_color_hex(ACCENT), 0);
        lv_obj_set_style_bg_opa(st->row_bar[i], LV_OPA_60, 0);
        lv_obj_set_style_radius(st->row_bar[i], 1, 0);
        lv_obj_clear_flag(st->row_bar[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(st->row_bar[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(st->row_bar[i], LV_ALIGN_TOP_RIGHT, -20, y0 + i * ROW_H + 18);
        lv_obj_add_flag(st->row_bar[i], LV_OBJ_FLAG_HIDDEN);
    }

    st->state_label = lv_label_create(parent);
    lv_obj_set_style_text_font(st->state_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->state_label, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->state_label, LV_OPA_60, 0);
    lv_label_set_text(st->state_label, "BOOT to scan");
    lv_obj_align(st->state_label, LV_ALIGN_BOTTOM_MID, 0, -54);

    st->mode_label = lv_label_create(parent);
    lv_obj_set_style_text_font(st->mode_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(st->mode_label, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->mode_label, LV_OPA_50, 0);
    lv_label_set_text(st->mode_label, "show: name  (USER to toggle)");
    lv_obj_align(st->mode_label, LV_ALIGN_BOTTOM_MID, 0, -34);
}

scene_t scene_sniff = {
    .id            = "sniff",
    .display_name  = "XVIII. Sniff",
    .description   = "BLE devices as data table (name/addr + RSSI + type)",
    .tags          = "ble,radio,data-table",
    .accent        = LV_COLOR_MAKE(0xE8, 0xC3, 0x87),
    .init          = sniff_init,
    .on_show       = sniff_on_show,
    .frame         = sniff_frame,
    .on_long_press = sniff_long_press,
};
