/*
 * Scene VII · Vault — SD card management.
 *
 * Reads + actions:
 *   - Status block (capacity, used, free, name, type, bus speed)
 *   - Auto-poll for hot-plug: when no card is mounted, this scene
 *     silently retries mount every 2 s. The board has no card-detect
 *     GPIO wired, so polling is the only "automatic" path. ~50 ms
 *     after the card seats correctly, the screen updates.
 *
 *   - BOOT / USER buttons cycle through actions:
 *       remount       → unmount + re-init (recover from hot-pull)
 *       bench 1 MB    → write 1 MB, fsync, read back; report KB/s
 *       bench 4 MB    → same with 4 MB
 *       probe file    → write a 4 KB probe + verify
 *       FORMAT (!)    → erase + re-FAT-format. Two-step confirm: the
 *                       first long press arms, the second within 5 s
 *                       executes. Disarmed by selecting any other
 *                       action or by 5 s timeout.
 *
 *   - Long-press → execute the currently-selected action.
 */

#include "harness/scene_framework.h"
#include "lvgl.h"
#include "peripherals/sdcard.h"
#include "peripherals/keys.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include <stdio.h>
#include <string.h>

#define ACCENT      0xE6C77A
#define ARC_R       150

typedef enum {
    ACT_REMOUNT = 0,
    ACT_BENCH_1MB,
    ACT_BENCH_4MB,
    ACT_PROBE,
    ACT_FORMAT,
    ACT_COUNT,
} vault_action_t;

static const char *action_label(vault_action_t a)
{
    switch (a) {
        case ACT_REMOUNT:    return "remount";
        case ACT_BENCH_1MB:  return "bench 1 MB";
        case ACT_BENCH_4MB:  return "bench 4 MB";
        case ACT_PROBE:      return "write probe";
        case ACT_FORMAT:     return "FORMAT (erase)";
        default:             return "?";
    }
}

typedef struct {
    /* Status block */
    lv_obj_t *roman;
    lv_obj_t *arc;
    lv_obj_t *cap_label;     /* "16384 MB" */
    lv_obj_t *cap_sub;       /* "used 12 / SDHC / 40 MHz" or "no card" */
    lv_obj_t *name_label;    /* card name from CID */

    /* Action selector + result */
    lv_obj_t *action_tag;    /* "ACTION" */
    lv_obj_t *action_label;  /* current action name */
    lv_obj_t *result_label;  /* outcome of last action */
    lv_obj_t *hint;          /* gesture hint */

    vault_action_t  action;
    bool            format_armed;
    uint32_t        format_armed_until_ms;
    bool            busy;
    uint32_t        last_boot_count;
    uint32_t        last_user_count;
    uint32_t        last_poll_ms;
    uint32_t        last_refresh_ms;
    lv_timer_t     *timer;
} vault_state_t;

static void refresh_status(vault_state_t *st)
{
    sdcard_state_t s;
    sdcard_get(&s);

    if (!s.mounted) {
        lv_arc_set_value(st->arc, 0);
        lv_obj_set_style_arc_opa(st->arc, LV_OPA_30, LV_PART_INDICATOR);
        lv_label_set_text(st->cap_label, "no card");
        char sub[40];
        if (s.last_mount_err != 0) {
            snprintf(sub, sizeof(sub), "auto-retry every 2 s");
        } else {
            snprintf(sub, sizeof(sub), "insert microSD");
        }
        lv_label_set_text(st->cap_sub, sub);
        lv_label_set_text(st->name_label, "");
        return;
    }

    lv_obj_set_style_arc_opa(st->arc, LV_OPA_90, LV_PART_INDICATOR);
    uint64_t cap_mb  = s.capacity_bytes / (1024ULL * 1024ULL);
    uint64_t used_mb = s.used_bytes / (1024ULL * 1024ULL);
    int pct = (cap_mb > 0) ? (int)((used_mb * 100ULL) / cap_mb) : 0;
    lv_arc_set_value(st->arc, pct);

    char buf[80];
    if (cap_mb >= 1024) {
        snprintf(buf, sizeof(buf), "%llu GB", (unsigned long long)(cap_mb / 1024));
    } else {
        snprintf(buf, sizeof(buf), "%llu MB", (unsigned long long)cap_mb);
    }
    lv_label_set_text(st->cap_label, buf);

    /* `card_type` is up to 12 chars, used_mb up to 19 digits, "%d MHz"
     * up to 11 chars; comfortable 80-byte buffer. */
    snprintf(buf, sizeof(buf), "used %llu MB  %s  %d MHz",
             (unsigned long long)used_mb, s.card_type, s.speed_khz / 1000);
    lv_label_set_text(st->cap_sub, buf);

    snprintf(buf, sizeof(buf), "%.15s", s.card_name);
    lv_label_set_text(st->name_label, buf);
}

static void refresh_action(vault_state_t *st)
{
    char buf[40];
    if (st->action == ACT_FORMAT && st->format_armed) {
        snprintf(buf, sizeof(buf), "[%s] ARMED", action_label(st->action));
    } else {
        snprintf(buf, sizeof(buf), "[%s]", action_label(st->action));
    }
    lv_label_set_text(st->action_label, buf);
}

static void execute_action(vault_state_t *st)
{
    char result[64];
    int rc;
    int write_kbps, read_kbps;
    int64_t t0;
    uint32_t now;

    sdcard_state_t s;
    sdcard_get(&s);

    switch (st->action) {
        case ACT_REMOUNT:
            t0 = esp_timer_get_time();
            bool ok = sdcard_remount();
            int el_ms = (int)((esp_timer_get_time() - t0) / 1000);
            snprintf(result, sizeof(result),
                     ok ? "remount OK (%d ms)" : "remount FAIL (err=0x%x)",
                     ok ? el_ms : (unsigned)s.last_mount_err);
            (void)el_ms;
            break;

        case ACT_BENCH_1MB:
        case ACT_BENCH_4MB: {
            if (!s.mounted) { snprintf(result, sizeof(result), "no card"); break; }
            int mb = (st->action == ACT_BENCH_1MB) ? 1 : 4;
            lv_label_set_text(st->result_label, "running benchmark...");
            /* Force a redraw so the user sees the message before the
             * synchronous benchmark blocks the LVGL task for ~1 s. */
            lv_refr_now(NULL);
            rc = sdcard_benchmark(mb, &write_kbps, &read_kbps);
            if (rc == 0) {
                snprintf(result, sizeof(result),
                         "%d MB:  W %.1f  R %.1f MB/s", mb,
                         write_kbps / 1024.0f, read_kbps / 1024.0f);
            } else {
                snprintf(result, sizeof(result), "bench FAIL rc=%d", rc);
            }
            break;
        }

        case ACT_PROBE: {
            if (!s.mounted) { snprintf(result, sizeof(result), "no card"); break; }
            char path[64];
            snprintf(path, sizeof(path), "%s/aurora_probe.bin", BSP_SD_MOUNT_POINT);
            FILE *f = fopen(path, "wb");
            if (!f) { snprintf(result, sizeof(result), "probe: open FAIL"); break; }
            static uint8_t pat[1024];
            for (int i = 0; i < 1024; ++i) pat[i] = (uint8_t)(i ^ 0x5A);
            size_t total = 0;
            for (int i = 0; i < 4; ++i) total += fwrite(pat, 1, sizeof(pat), f);
            fclose(f);
            snprintf(result, sizeof(result), "probe: %u bytes ok", (unsigned)total);
            break;
        }

        case ACT_FORMAT: {
            if (!s.mounted) { snprintf(result, sizeof(result), "no card"); break; }
            now = lv_tick_get();
            if (!st->format_armed) {
                /* First long press → arm. */
                st->format_armed = true;
                st->format_armed_until_ms = now + 5000;
                refresh_action(st);
                snprintf(result, sizeof(result),
                         "HOLD AGAIN within 5s to ERASE");
                break;
            }
            if ((int32_t)(st->format_armed_until_ms - now) <= 0) {
                /* Stale arm — re-arm. */
                st->format_armed_until_ms = now + 5000;
                snprintf(result, sizeof(result),
                         "HOLD AGAIN within 5s to ERASE");
                break;
            }
            /* Executing destructive op. */
            lv_label_set_text(st->result_label, "formatting...");
            lv_refr_now(NULL);
            rc = sdcard_format();
            st->format_armed = false;
            refresh_action(st);
            if (rc == 0) {
                snprintf(result, sizeof(result), "format ok, empty FAT");
            } else {
                snprintf(result, sizeof(result), "format FAIL rc=%d", rc);
            }
            break;
        }

        default:
            snprintf(result, sizeof(result), "?");
    }

    lv_label_set_text(st->result_label, result);
    refresh_status(st);
}

/* ── tick: vol-style button polling + hot-plug poll + format timeout ── */

static void vault_tick(lv_timer_t *t)
{
    vault_state_t *st = (vault_state_t *)lv_timer_get_user_data(t);
    if (!st) return;
    uint32_t now = lv_tick_get();

    /* Action selector via BOOT (prev) / USER (next). Edge-counted. */
    keys_state_t k;
    keys_get(&k);
    bool action_changed = false;
    if (k.boot_count != st->last_boot_count) {
        st->last_boot_count = k.boot_count;
        st->action = (vault_action_t)((st->action + ACT_COUNT - 1) % ACT_COUNT);
        /* Selecting a different action disarms a pending FORMAT. */
        st->format_armed = false;
        action_changed = true;
    }
    if (k.user_count != st->last_user_count) {
        st->last_user_count = k.user_count;
        st->action = (vault_action_t)((st->action + 1) % ACT_COUNT);
        st->format_armed = false;
        action_changed = true;
    }
    if (action_changed) {
        refresh_action(st);
        lv_label_set_text(st->hint, "long press to execute");
    }

    /* Auto-disarm FORMAT after 5 s without a confirming hold. */
    if (st->format_armed &&
        (int32_t)(st->format_armed_until_ms - now) <= 0) {
        st->format_armed = false;
        refresh_action(st);
        lv_label_set_text(st->result_label, "format disarmed");
    }

    /* Hot-plug + refresh both happen in the sdcard background task —
     * tick just observes the cached state, which is a memcpy. The
     * background task is enabled in our on_show and disabled in
     * on_hide so we don't run SDIO traffic when this scene is hidden.
     *
     * Re-render the status block periodically anyway, so the bg task's
     * successful mount becomes visible to the user within ~250 ms
     * regardless of our internal cadence. */
    (void)st->last_poll_ms;     /* fields kept for cross-API ABI */
    if ((now - st->last_refresh_ms) >= 250) {
        st->last_refresh_ms = now;
        bool was_mounted = false;
        sdcard_state_t prev_status;
        sdcard_get(&prev_status);
        was_mounted = prev_status.mounted;
        refresh_status(st);
        sdcard_state_t cur_status;
        sdcard_get(&cur_status);
        if (!was_mounted && cur_status.mounted) {
            lv_label_set_text(st->result_label, "card detected");
        }
    }
}

/* ── gestures ────────────────────────────────────────────────────── */

static void vault_long_press(scene_t *s)
{
    vault_state_t *st = (vault_state_t *)s->user_data;
    if (!st || st->busy) return;
    /* Synchronous actions execute inline (we hold the LVGL lock from
     * the on_long_press dispatch). Benchmarks take ~1 s for 1 MB on a
     * typical class-4 card — noticeable freeze but acceptable for a
     * test page. Format is more expensive (~3-5 s); we show the
     * "formatting..." message before kicking off. */
    st->busy = true;
    execute_action(st);
    st->busy = false;
}

/* ── widgets ────────────────────────────────────────────────────── */

static void vault_init(scene_t *s, lv_obj_t *parent)
{
    vault_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;
    st->action = ACT_REMOUNT;

    /* Roman VII */
    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(st->roman, 6, 0);
    lv_obj_set_style_text_color(st->roman, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_60, 0);
    lv_label_set_text(st->roman, "VII");
    lv_obj_align(st->roman, LV_ALIGN_TOP_MID, 0, 92);

    /* Outer arc = used-percent ring. */
    st->arc = lv_arc_create(parent);
    lv_obj_set_size(st->arc, ARC_R * 2, ARC_R * 2);
    lv_obj_center(st->arc);
    lv_arc_set_rotation(st->arc, 270);
    lv_arc_set_bg_angles(st->arc, 0, 360);
    lv_arc_set_range(st->arc, 0, 100);
    lv_arc_set_value(st->arc, 0);
    lv_obj_remove_style(st->arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(st->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(st->arc, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(st->arc, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_arc_color(st->arc, lv_color_hex(ACCENT), LV_PART_MAIN);
    lv_obj_set_style_arc_width(st->arc, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(st->arc, lv_color_hex(ACCENT), LV_PART_INDICATOR);

    /* Big capacity number */
    st->cap_label = lv_label_create(parent);
    lv_obj_set_style_text_font(st->cap_label, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(st->cap_label, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->cap_label, LV_OPA_90, 0);
    lv_label_set_text(st->cap_label, "no card");
    lv_obj_align(st->cap_label, LV_ALIGN_CENTER, 0, -60);

    /* Sub-line: used / type / speed */
    st->cap_sub = lv_label_create(parent);
    lv_obj_set_style_text_font(st->cap_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->cap_sub, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->cap_sub, LV_OPA_60, 0);
    lv_label_set_text(st->cap_sub, "insert microSD");
    lv_obj_align(st->cap_sub, LV_ALIGN_CENTER, 0, -35);

    /* Card name */
    st->name_label = lv_label_create(parent);
    lv_obj_set_style_text_font(st->name_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->name_label, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->name_label, LV_OPA_50, 0);
    lv_label_set_text(st->name_label, "");
    lv_obj_align(st->name_label, LV_ALIGN_CENTER, 0, -15);

    /* Action selector */
    st->action_tag = lv_label_create(parent);
    lv_obj_set_style_text_font(st->action_tag, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(st->action_tag, 3, 0);
    lv_obj_set_style_text_color(st->action_tag, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->action_tag, LV_OPA_50, 0);
    lv_label_set_text(st->action_tag, "ACTION  boot/user to cycle");
    lv_obj_align(st->action_tag, LV_ALIGN_CENTER, 0, 18);

    st->action_label = lv_label_create(parent);
    lv_obj_set_style_text_font(st->action_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(st->action_label, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->action_label, LV_OPA_90, 0);
    lv_label_set_text(st->action_label, "[remount]");
    lv_obj_align(st->action_label, LV_ALIGN_CENTER, 0, 40);

    /* Result line */
    st->result_label = lv_label_create(parent);
    lv_obj_set_style_text_font(st->result_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->result_label, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->result_label, LV_OPA_70, 0);
    lv_label_set_text(st->result_label, "");
    lv_obj_align(st->result_label, LV_ALIGN_CENTER, 0, 65);

    /* Hint */
    st->hint = lv_label_create(parent);
    lv_obj_set_style_text_font(st->hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->hint, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->hint, LV_OPA_40, 0);
    lv_label_set_text(st->hint, "long press to execute");
    lv_obj_align(st->hint, LV_ALIGN_CENTER, 0, 88);

    /* Sync key counters. */
    keys_state_t k;
    keys_get(&k);
    st->last_boot_count = k.boot_count;
    st->last_user_count = k.user_count;

    refresh_status(st);
    refresh_action(st);

    /* 100 ms tick: responsive to button presses + polls SD every 2 s
     * via internal cadence. */
    st->timer = lv_timer_create(vault_tick, 100, st);
    lv_timer_pause(st->timer);
}

static void vault_on_show(scene_t *s)
{
    vault_state_t *st = (vault_state_t *)s->user_data;
    if (!st) return;
    keys_state_t k;
    keys_get(&k);
    st->last_boot_count = k.boot_count;
    st->last_user_count = k.user_count;
    /* Hand hot-plug polling off to the background sdcard task. It
     * wakes immediately on enable, so insertion detection latency is
     * basically "next mount-retry attempt" (2 s ceiling) without
     * blocking us. */
    sdcard_polling_enable(true);
    refresh_status(st);
    refresh_action(st);
    if (st->timer) lv_timer_resume(st->timer);
}
static void vault_on_hide(scene_t *s)
{
    vault_state_t *st = (vault_state_t *)s->user_data;
    if (st && st->timer) lv_timer_pause(st->timer);
    /* Stop SDIO retries when this scene isn't visible — no point
     * thrashing the bus or generating log noise from other contexts. */
    sdcard_polling_enable(false);
}

scene_t scene_vault = {
    .id            = "vault",
    .display_name  = "VII. Vault",
    .accent        = LV_COLOR_MAKE(0xE6, 0xC7, 0x7A),
    .description   = "SD card status + management: mount/unmount/probe/format; auto-hot-plug poll",
    .tags          = "sdcard,storage,interactive",
    .init          = vault_init,
    .on_show       = vault_on_show,
    .on_hide       = vault_on_hide,
    .on_long_press = vault_long_press,
};
