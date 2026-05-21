/*
 * Scene XVII · Survey — WiFi APs, tabular.
 *
 * Complement to Scene IX (Spectrum). Spectrum is art (RSSI -> radial
 * dots arranged by channel); Survey is data (top-8 list with SSID,
 * RSSI bar, channel, auth). Same backend (peripherals/wifi.c), two
 * presentations.
 *
 * Controls:
 *   short tap: next scene
 *   long press: trigger scan (frees BLE first if needed, like Spectrum)
 *   BOOT key edge: same as long press (refresh scan)
 *   USER key edge: toggle sort (rssi-desc <-> channel-asc)
 *
 * Coexistence inherited from Spectrum: first scan releases BLE, BLE
 * scan returns -1 until reboot.
 */

#include "harness/scene_framework.h"
#include "lvgl.h"
#include "peripherals/wifi.h"
#include "peripherals/ble.h"
#include "peripherals/keys.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define ACCENT       0xA9C8DC
#define ROW_COUNT    8
#define ROW_H        24
#define BAR_W        70

typedef enum {
    SORT_RSSI = 0,
    SORT_CHANNEL = 1,
} survey_sort_t;

typedef struct {
    lv_obj_t *roman;
    lv_obj_t *header;
    lv_obj_t *state_label;
    lv_obj_t *sort_label;
    lv_obj_t *row_ssid[ROW_COUNT];
    lv_obj_t *row_meta[ROW_COUNT];
    lv_obj_t *row_bar[ROW_COUNT];

    wifi_ap_t aps[WIFI_SCAN_MAX_RESULTS];
    int       ap_count;
    int       elapsed_ms;
    bool      scanning;
    survey_sort_t sort;

    int       last_boot_count;
    int       last_user_count;
} survey_state_t;

static int cmp_rssi_desc(const void *a, const void *b)
{
    return ((const wifi_ap_t *)b)->rssi - ((const wifi_ap_t *)a)->rssi;
}
static int cmp_channel_asc(const void *a, const void *b)
{
    return (int)((const wifi_ap_t *)a)->channel - (int)((const wifi_ap_t *)b)->channel;
}

static void render_rows(survey_state_t *st)
{
    /* Sort in place. wifi_scan already returned rssi-desc; for channel
     * we re-sort, then this function applies it. */
    if (st->ap_count > 1) {
        if (st->sort == SORT_RSSI)
            qsort(st->aps, st->ap_count, sizeof(wifi_ap_t), cmp_rssi_desc);
        else
            qsort(st->aps, st->ap_count, sizeof(wifi_ap_t), cmp_channel_asc);
    }

    for (int i = 0; i < ROW_COUNT; ++i) {
        if (i < st->ap_count) {
            const wifi_ap_t *a = &st->aps[i];
            /* SSID column: truncate to 18 chars */
            char ssid[20];
            snprintf(ssid, sizeof(ssid), "%.18s", a->ssid[0] ? a->ssid : "<hidden>");
            lv_label_set_text(st->row_ssid[i], ssid);

            /* Meta column: "ch11  -47 wpa2" */
            char meta[24];
            snprintf(meta, sizeof(meta), "ch%-2d  %4d  %s",
                     (int)a->channel, (int)a->rssi,
                     wifi_auth_label(a->authmode));
            lv_label_set_text(st->row_meta[i], meta);

            /* RSSI bar width: -30 dBm = full, -100 dBm = empty */
            int rssi = (int)a->rssi;
            if (rssi > -30)  rssi = -30;
            if (rssi < -100) rssi = -100;
            int w = (BAR_W * (-30 - rssi)) / -70;
            w = BAR_W - w;
            if (w < 2) w = 2;
            lv_obj_set_width(st->row_bar[i], w);
            lv_obj_clear_flag(st->row_bar[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(st->row_ssid[i], "");
            lv_label_set_text(st->row_meta[i], "");
            lv_obj_add_flag(st->row_bar[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    char b[32];
    snprintf(b, sizeof(b), "%d AP / %d ms", st->ap_count, st->elapsed_ms);
    lv_label_set_text(st->state_label, b);
    lv_label_set_text(st->sort_label,
                      st->sort == SORT_RSSI ? "sort: rssi" : "sort: chan");
}

typedef struct {
    survey_state_t *st;
    wifi_ap_t      aps[WIFI_SCAN_MAX_RESULTS];
    int            count;
    int            elapsed_ms;
} survey_scan_t;

static void apply_scan(void *arg)
{
    survey_scan_t *r = (survey_scan_t *)arg;
    if (!r) return;
    survey_state_t *st = r->st;
    if (!st) { lv_free(r); return; }
    memcpy(st->aps, r->aps, sizeof(st->aps));
    st->ap_count = r->count;
    st->elapsed_ms = r->elapsed_ms;
    st->scanning = false;
    render_rows(st);
    lv_free(r);
}

static void scan_task(void *arg)
{
    survey_scan_t *r = (survey_scan_t *)arg;
    int64_t t0 = esp_timer_get_time();
    r->count = wifi_scan(r->aps, WIFI_SCAN_MAX_RESULTS, 600);
    r->elapsed_ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (r->count < 0) r->count = 0;
    lv_async_call(apply_scan, r);
    vTaskDelete(NULL);
}

static void start_scan(survey_state_t *st)
{
    if (!st || st->scanning) return;
    st->scanning = true;
    lv_label_set_text(st->state_label, "scanning...");

    /* Same coexistence dance as Spectrum: if BLE is up, tear it down
     * first so WiFi can grab the SRAM pool. */
    if (ble_is_up()) {
        lv_label_set_text(st->state_label, "releasing ble...");
        ble_deinit();
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    survey_scan_t *r = lv_malloc_zeroed(sizeof(*r));
    if (!r) { st->scanning = false; return; }
    r->st = st;
    if (xTaskCreate(scan_task, "survey_scan", 4096, r, 5, NULL) != pdPASS) {
        lv_free(r);
        st->scanning = false;
        lv_label_set_text(st->state_label, "task create failed");
    }
}

static void survey_long_press(scene_t *s)
{
    start_scan((survey_state_t *)s->user_data);
}

static void survey_frame(scene_t *s, uint32_t t_ms)
{
    (void)t_ms;
    survey_state_t *st = (survey_state_t *)s->user_data;
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
            st->sort = (st->sort == SORT_RSSI) ? SORT_CHANNEL : SORT_RSSI;
            render_rows(st);
        }
    }
}

static void survey_on_show(scene_t *s)
{
    survey_state_t *st = (survey_state_t *)s->user_data;
    if (!st) return;
    keys_state_t k;
    keys_get(&k);
    st->last_boot_count = (int)k.boot_count;
    st->last_user_count = (int)k.user_count;
}

static void survey_init(scene_t *s, lv_obj_t *parent)
{
    survey_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;
    st->sort = SORT_RSSI;

    /* Roman XVII */
    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(st->roman, 4, 0);
    lv_obj_set_style_text_color(st->roman, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_50, 0);
    lv_label_set_text(st->roman, "XVII");
    lv_obj_align(st->roman, LV_ALIGN_TOP_MID, 0, 28);

    /* Header */
    st->header = lv_label_create(parent);
    lv_obj_set_style_text_font(st->header, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->header, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->header, LV_OPA_60, 0);
    lv_label_set_text(st->header, "SURVEY  /  WiFi APs");
    lv_obj_align(st->header, LV_ALIGN_TOP_MID, 0, 60);

    /* Row table */
    const int y0 = 100;
    for (int i = 0; i < ROW_COUNT; ++i) {
        /* SSID label (left) */
        st->row_ssid[i] = lv_label_create(parent);
        lv_obj_set_style_text_font(st->row_ssid[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(st->row_ssid[i], lv_color_white(), 0);
        lv_obj_set_style_text_opa(st->row_ssid[i], LV_OPA_90, 0);
        lv_label_set_text(st->row_ssid[i], "");
        lv_obj_align(st->row_ssid[i], LV_ALIGN_TOP_LEFT, 20, y0 + i * ROW_H);

        /* Meta label (right) */
        st->row_meta[i] = lv_label_create(parent);
        lv_obj_set_style_text_font(st->row_meta[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(st->row_meta[i], lv_color_hex(ACCENT), 0);
        lv_obj_set_style_text_opa(st->row_meta[i], LV_OPA_70, 0);
        lv_label_set_text(st->row_meta[i], "");
        lv_obj_align(st->row_meta[i], LV_ALIGN_TOP_RIGHT, -20, y0 + i * ROW_H + 2);

        /* RSSI bar (below the row) */
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

    /* Status row at the bottom */
    st->state_label = lv_label_create(parent);
    lv_obj_set_style_text_font(st->state_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->state_label, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->state_label, LV_OPA_60, 0);
    lv_label_set_text(st->state_label, "BOOT to scan");
    lv_obj_align(st->state_label, LV_ALIGN_BOTTOM_MID, 0, -54);

    st->sort_label = lv_label_create(parent);
    lv_obj_set_style_text_font(st->sort_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(st->sort_label, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->sort_label, LV_OPA_50, 0);
    lv_label_set_text(st->sort_label, "sort: rssi  (USER to toggle)");
    lv_obj_align(st->sort_label, LV_ALIGN_BOTTOM_MID, 0, -34);
}

scene_t scene_survey = {
    .id            = "survey",
    .display_name  = "XVII. Survey",
    .description   = "WiFi APs as data table (SSID + ch + RSSI + auth)",
    .tags          = "wifi,radio,data-table",
    .accent        = LV_COLOR_MAKE(0xA9, 0xC8, 0xDC),
    .init          = survey_init,
    .on_show       = survey_on_show,
    .frame         = survey_frame,
    .on_long_press = survey_long_press,
};
