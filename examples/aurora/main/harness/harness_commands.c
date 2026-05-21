/*
 * harness_commands — concrete commands that close the AI feedback loop.
 *
 * Implementation notes:
 *  · All commands run on the console parser task. Anything that touches LVGL
 *    state must hold lv_lock()/lv_unlock() (or be dispatched via lv_async_call).
 *  · ?dump is the only long-running command. It allocates a transient
 *    downsample buffer in PSRAM, base64-encodes inline, and writes in chunks
 *    via printf so output is human-tail-able.
 */

#include "harness_commands.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "lvgl.h"
#include "bsp/esp-bsp.h"  /* full BSP API including bsp_display_lock/unlock */

#include "harness/console_protocol.h"
#include "harness/scene_framework.h"
/* ui_shell.h previously used by cmd_scene's chrome sync; that lifted to
 * aurora-harness with a listener interface in v1.4 — main is the listener
 * registrant in aurora_main.c, harness_commands itself no longer touches
 * ui_shell directly. */
#include "peripherals/imu.h"
#include "peripherals/pmic.h"
#include "peripherals/audio.h"
#include "peripherals/sdcard.h"
#include "peripherals/wifi.h"
#include "peripherals/ble.h"
#include "peripherals/keys.h"
#include "peripherals/system.h"

#define TAG "harness_cmd"

/* ── ?stat + frame counter moved to aurora-harness component ───────
 * v1.2 lifted these into components/aurora-harness/src/default_cmds.c.
 * Consumers now call harness_default_register() once + tick
 * harness_record_frame() from their own LVGL frame timer.
 */
#include "harness/default_cmds.h"

/* ── ?dump moved to aurora-harness/src/screenshot.c (v1.4) ───────── */
#if 0  /* legacy body preserved in git history; kept commented while v1.4 stabilises */
static lv_draw_buf_t *s_snap_buf = NULL;
static int32_t        s_snap_w   = 0;
static int32_t        s_snap_h   = 0;

/* Forward-declared helpers that need to see the static state above. */
static SemaphoreHandle_t s_dump_done_sem;
static lv_result_t       s_dump_async_result;

/* Composite a top-layer ARGB8888 buffer over a screen RGB565 buffer.
 * Where top.a > 0, blend top RGB into screen. Done in-place on screen_buf.
 * This is how the UI shell (label + dots + fps) — which lives on
 * lv_layer_top() — gets included in the dump. lv_snapshot_take on the
 * active screen alone misses the top layer. */
static void composite_top_over_screen(lv_draw_buf_t *screen, lv_draw_buf_t *top)
{
    int32_t W = screen->header.w;
    int32_t H = screen->header.h;
    int32_t s_stride_px = screen->header.stride / 2;     /* RGB565 */
    int32_t t_stride_px = top->header.stride / 4;        /* ARGB8888 */
    uint16_t *sbuf = (uint16_t *)screen->data;
    uint32_t *tbuf = (uint32_t *)top->data;

    for (int y = 0; y < H; ++y) {
        uint16_t *srow = sbuf + (size_t)y * s_stride_px;
        uint32_t *trow = tbuf + (size_t)y * t_stride_px;
        for (int x = 0; x < W; ++x) {
            uint32_t tp = trow[x];
            uint8_t a = (uint8_t)((tp >> 24) & 0xFF);
            if (a == 0) continue;
            uint8_t tr = (uint8_t)((tp >> 16) & 0xFF);
            uint8_t tg = (uint8_t)((tp >>  8) & 0xFF);
            uint8_t tb = (uint8_t)( tp        & 0xFF);
            if (a == 0xFF) {
                srow[x] = (uint16_t)(((tr & 0xF8) << 8) |
                                      ((tg & 0xFC) << 3) |
                                      ((tb & 0xF8) >> 3));
            } else {
                uint16_t s = srow[x];
                uint32_t sr = ((s >> 11) & 0x1F) << 3;
                uint32_t sg = ((s >>  5) & 0x3F) << 2;
                uint32_t sb = ( s        & 0x1F) << 3;
                uint32_t r = (tr * a + sr * (255 - a)) / 255;
                uint32_t g = (tg * a + sg * (255 - a)) / 255;
                uint32_t b = (tb * a + sb * (255 - a)) / 255;
                srow[x] = (uint16_t)(((r & 0xF8) << 8) |
                                      ((g & 0xFC) << 3) |
                                      ((b & 0xF8) >> 3));
            }
        }
    }
}

/* Runs on the LVGL task, called via lv_async_call. It already has the
 * implicit LVGL state lock so it can safely traverse the widget tree. */
void aurora_do_snapshot(void *user)
{
    (void)user;
    /* 1. Snapshot the active screen (scene content). */
    s_dump_async_result = lv_snapshot_take_to_draw_buf(
        lv_screen_active(), LV_COLOR_FORMAT_RGB565, s_snap_buf);

    /* 2. Snapshot the top layer (UI shell) and composite over the screen.
     * Allocated transiently each dump — ~870 KB ARGB8888 at 466×466 fits
     * comfortably in PSRAM. Failure is non-fatal: the chrome just won't
     * appear in this dump, which is better than failing the whole dump. */
    if (s_dump_async_result == LV_RESULT_OK) {
        lv_draw_buf_t *top = lv_snapshot_take(lv_layer_top(),
                                               LV_COLOR_FORMAT_ARGB8888);
        if (top != NULL) {
            composite_top_over_screen(s_snap_buf, top);
            lv_draw_buf_destroy(top);
        }
    }

    if (s_dump_done_sem) xSemaphoreGive(s_dump_done_sem);
}

static int cmd_dump(const console_args_t *args)
{
    /* Parse optional w=N */
    int target_w = 128;
    for (int i = 1; i < args->argc; ++i) {
        const char *a = args->argv[i];
        if (strncmp(a, "w=", 2) == 0) {
            int v = atoi(a + 2);
            if (v >= 32 && v <= 256) target_w = v;
        }
    }
    int target_h = target_w;

    lv_obj_t *scr = lv_screen_active();
    int32_t src_w = lv_obj_get_width(scr);
    int32_t src_h = lv_obj_get_height(scr);
    if (src_w <= 0 || src_h <= 0) {
        console_reply_err("invalid screen size %dx%d", (int)src_w, (int)src_h);
        return 1;
    }

    /* (Re)allocate persistent snapshot buffer in PSRAM the first time, or
     * if the screen size changed. lv_draw_buf_create allocates via
     * lv_malloc — which under our config is libc malloc, which in turn
     * uses heap_caps_malloc with whatever caps the heap has. To force
     * PSRAM we sidestep that and pre-allocate ourselves. */
    if (s_snap_buf == NULL || s_snap_w != src_w || s_snap_h != src_h) {
        if (s_snap_buf) {
            lv_draw_buf_destroy(s_snap_buf);
            s_snap_buf = NULL;
        }
        size_t need = (size_t)src_w * (size_t)src_h * 2;  /* RGB565 */
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < need + 16 * 1024) {
            console_reply_err("psram low: need %u, free %u",
                              (unsigned)need,
                              (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            return 1;
        }
        s_snap_buf = lv_draw_buf_create(src_w, src_h,
                                         LV_COLOR_FORMAT_RGB565,
                                         LV_STRIDE_AUTO);
        if (!s_snap_buf) {
            console_reply_err("lv_draw_buf_create failed (need %u)", (unsigned)need);
            return 1;
        }
        s_snap_w = src_w;
        s_snap_h = src_h;
    }

    /* Take the snapshot via lv_async_call so the snapshot runs INSIDE the
     * LVGL task — which already holds the LVGL mutex between frame timers.
     * Trying to grab the lock from the console task races the BSP's LVGL
     * adapter task and starves at 60 fps when scenes are active. */
    if (s_dump_done_sem == NULL) s_dump_done_sem = xSemaphoreCreateBinary();
    xSemaphoreTake(s_dump_done_sem, 0);  /* drain stale give */

    lv_async_call(aurora_do_snapshot, NULL);

    if (xSemaphoreTake(s_dump_done_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
        console_reply_err("snapshot timed out (LVGL task not draining)");
        return 1;
    }
    if (s_dump_async_result != LV_RESULT_OK) {
        console_reply_err("snapshot failed: %d", (int)s_dump_async_result);
        return 1;
    }
    lv_draw_buf_t *snap = s_snap_buf;

    /* Allocate downsampled buffer in PSRAM. (Note: do NOT free `snap` —
     * it's the cached s_snap_buf reused across dumps.) */
    size_t out_px = (size_t)target_w * (size_t)target_h;
    size_t out_bytes = out_px * 2;  /* RGB565 */
    uint16_t *small = heap_caps_malloc(out_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!small) {
        console_reply_err("psram alloc %u bytes failed", (unsigned)out_bytes);
        return 1;
    }

    /* Box-filter downsample: for each output pixel, average all source
     * pixels whose centres fall in its bin. We average in 8-bit channel
     * space (after bit-rep expand from 5/6) for perceptual correctness,
     * then re-quantise to RGB565 for the wire.
     *
     * Why: nearest-neighbour on an anti-aliased thin ring produces
     * "rainbow speckles" — each output pixel point-samples a different
     * blended source pixel, and the RGB565 quantisation grid in 5/6/5
     * doesn't preserve hue, so neighbouring AA pixels round to different
     * colours. Averaging blends them back. */
    const uint16_t *src = (const uint16_t *)snap->data;
    int32_t stride_px = snap->header.stride / 2;
    for (int y = 0; y < target_h; ++y) {
        int sy_lo = (int)((int64_t)y * src_h / target_h);
        int sy_hi = (int)((int64_t)(y + 1) * src_h / target_h);
        if (sy_hi <= sy_lo) sy_hi = sy_lo + 1;
        if (sy_hi > src_h)  sy_hi = src_h;
        uint16_t *drow = small + (size_t)y * target_w;
        for (int x = 0; x < target_w; ++x) {
            int sx_lo = (int)((int64_t)x * src_w / target_w);
            int sx_hi = (int)((int64_t)(x + 1) * src_w / target_w);
            if (sx_hi <= sx_lo) sx_hi = sx_lo + 1;
            if (sx_hi > src_w)  sx_hi = src_w;

            uint32_t rs = 0, gs = 0, bs = 0;
            uint32_t n = 0;
            for (int sy = sy_lo; sy < sy_hi; ++sy) {
                const uint16_t *srow = src + (size_t)sy * stride_px;
                for (int sx = sx_lo; sx < sx_hi; ++sx) {
                    uint16_t v = srow[sx];
                    uint32_t r5 = (v >> 11) & 0x1F;
                    uint32_t g6 = (v >>  5) & 0x3F;
                    uint32_t b5 =  v        & 0x1F;
                    rs += (r5 << 3) | (r5 >> 2);
                    gs += (g6 << 2) | (g6 >> 4);
                    bs += (b5 << 3) | (b5 >> 2);
                    n++;
                }
            }
            uint32_t r8 = rs / n, g8 = gs / n, b8 = bs / n;
            drow[x] = (uint16_t)(((r8 & 0xF8) << 8) |
                                  ((g8 & 0xFC) << 3) |
                                  ((b8 & 0xF8) >> 3));
        }
    }
    /* snap is the cached s_snap_buf — keep it for next dump. */

    /* Emit. Bytes: (target_w*target_h*2). Format: RGB565 little-endian per
     * pixel (matches LVGL native on S3). */
    char meta[80];
    snprintf(meta, sizeof(meta), "w=%d h=%d fmt=RGB565LE bytes=%u",
             target_w, target_h, (unsigned)out_bytes);
    console_reply_ok("dump start %s", meta);
    console_begin_payload("DUMP", meta);

    /* Encode + emit in 48-byte (16-triplet) input chunks → 64-char base64
     * output lines. 64 chars/line keeps total output sane and easy to parse. */
    const uint8_t *p = (const uint8_t *)small;
    size_t left = out_bytes;
    char line[72];
    while (left > 0) {
        size_t chunk = left >= 48 ? 48 : left;
        int line_len = 0;
        size_t i = 0;
        while (i < chunk) {
            size_t n = (chunk - i) >= 3 ? 3 : (chunk - i);
            b64_encode_chunk(p + i, n, &line[line_len]);
            line_len += 4;
            i += n;
        }
        line[line_len++] = '\n';
        fwrite(line, 1, line_len, stdout);
        p += chunk;
        left -= chunk;
    }
    fflush(stdout);
    console_end_payload("DUMP");

    free(small);
    return 0;
}
#endif  /* legacy cmd_dump body — see aurora-harness/src/screenshot.c */

/* ── tap / swipe moved to aurora-harness/default_cmds.c (v1.4) ─────
 * Pure LVGL synthetic-touch helpers; no peripheral coupling. */

/* ── scene control moved to aurora-harness/default_cmds.c (v1.4) ─────
 * Application UI chrome (ui_shell_set_active in aurora_main) listens via
 * scene_fw_set_change_listener — no longer hard-coded inside cmd_scene. */

/* ── Registration ──────────────────────────────────────────────────── */

static int cmd_sensor(const console_args_t *args)
{
    (void)args;
    float ax, ay, az, gx, gy, gz;
    imu_get_accel(&ax, &ay, &az);
    imu_get_gyro(&gx, &gy, &gz);
    float tc = imu_get_temp_c();
    /* Return values regardless of imu_is_ready() — caller can read the
     * "ready" field to decide whether the numbers are meaningful. */
    char buf[260];
    snprintf(buf, sizeof(buf),
             "{\"ready\":%s,\"accel\":[%.3f,%.3f,%.3f],"
             "\"gyro\":[%.2f,%.2f,%.2f],\"temp_c\":%.1f}",
             imu_is_ready() ? "true" : "false",
             ax, ay, az, gx, gy, gz, tc);
    console_reply_ok("%s", buf);
    return 0;
}

/* ── ?sys ────────────────────────────────────────────────────────── */

static int cmd_sys(const console_args_t *args)
{
    (void)args;
    system_info_t s;
    system_get(&s);
    /* Big JSON; use printf framing into one OK: line. Helper buf sized
     * for typical content (~480 bytes). */
    char buf[600];
    snprintf(buf, sizeof(buf),
             "{\"cpu_mhz\":%d,\"cores\":%d,\"chip_rev\":%d,"
             "\"sram_free\":%lu,\"sram_largest\":%lu,\"sram_min\":%lu,"
             "\"psram_free\":%lu,\"psram_largest\":%lu,\"psram_min\":%lu,"
             "\"flash_mb\":%lu,"
             "\"soc_temp_c\":%.1f,"
             "\"uptime_ms\":%llu,"
             "\"wifi_mac\":\"%s\",\"bt_mac\":\"%s\","
             "\"idf\":\"%s\",\"app\":\"%s\",\"version\":\"%s\","
             "\"reset_reason\":\"%s\",\"elf_sha\":\"%s\"}",
             s.cpu_freq_mhz, s.chip_cores, s.chip_revision,
             (unsigned long)s.heap_internal_free,
             (unsigned long)s.heap_internal_largest,
             (unsigned long)s.heap_internal_min,
             (unsigned long)s.heap_psram_free,
             (unsigned long)s.heap_psram_largest,
             (unsigned long)s.heap_psram_min,
             (unsigned long)s.flash_size_mb,
             s.soc_temp_c,
             (unsigned long long)s.uptime_ms,
             s.wifi_mac, s.bt_mac,
             s.idf_version, s.app_name, s.app_version,
             s.reset_reason, s.elf_sha256_short);
    console_reply_ok("%s", buf);
    return 0;
}

/* ── radio (BLE/WiFi coexistence toggle) ─────────────────────────── */

static int cmd_radio(const console_args_t *args)
{
    if (args->argc < 2) {
        console_reply_ok("{\"ble_up\":%s}", ble_is_up() ? "true" : "false");
        return 0;
    }
    const char *sub = args->argv[1];
    if (strcmp(sub, "ble") == 0) {
        if (!ble_is_up()) {
            if (!ble_init()) {
                console_reply_err("ble_init failed");
                return 1;
            }
        }
        console_reply_ok("{\"mode\":\"ble\",\"ble_up\":true}");
        return 0;
    }
    if (strcmp(sub, "wifi") == 0) {
        if (ble_is_up()) {
            ble_deinit();
            vTaskDelay(pdMS_TO_TICKS(80));
        }
        /* Don't init wifi here — it lazy-inits on first wifi_scan call.
         * Just signal that the BLE pool has been released. */
        console_reply_ok("{\"mode\":\"wifi\",\"ble_up\":false}");
        return 0;
    }
    console_reply_err("radio: subcommand must be 'ble' or 'wifi' (no arg = status)");
    return 1;
}

/* ── ble ──────────────────────────────────────────────────────────── */

static int cmd_ble(const console_args_t *args)
{
    if (args->argc < 2 || strcmp(args->argv[1], "scan") != 0) {
        console_reply_err("usage: ble scan [DUR_MS [MAX_N]]");
        return 1;
    }
    int dur   = args->argc >= 3 ? atoi(args->argv[2]) : 2000;
    int max_n = args->argc >= 4 ? atoi(args->argv[3]) : 10;
    if (max_n < 1) max_n = 1;
    if (max_n > BLE_SCAN_MAX_DEVICES) max_n = BLE_SCAN_MAX_DEVICES;

    ble_device_t devs[BLE_SCAN_MAX_DEVICES];
    int adv_count = 0;
    int64_t t0 = esp_timer_get_time();
    int n = ble_scan(devs, max_n, dur, &adv_count);
    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;

    if (n < 0) {
        console_reply_err("ble_scan failed (n=%d, elapsed=%lldms)", n, elapsed_ms);
        return 1;
    }

    char buf[1400];
    int off = 0;
    off += snprintf(buf + off, sizeof(buf) - off,
                    "{\"count\":%d,\"adv_events\":%d,\"elapsed_ms\":%lld,\"devices\":[",
                    n, adv_count, elapsed_ms);
    for (int i = 0; i < n && off < (int)sizeof(buf) - 100; ++i) {
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%s{\"addr\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"rssi\":%d,\"name\":\"%s\"}",
                        i == 0 ? "" : ",",
                        devs[i].addr[5], devs[i].addr[4], devs[i].addr[3],
                        devs[i].addr[2], devs[i].addr[1], devs[i].addr[0],
                        (int)devs[i].rssi,
                        devs[i].name);
    }
    snprintf(buf + off, sizeof(buf) - off, "]}");
    console_reply_ok("%s", buf);
    return 0;
}

/* ── wifi ─────────────────────────────────────────────────────────── */

static int cmd_wifi(const console_args_t *args)
{
    if (args->argc < 2) {
        console_reply_err("usage: wifi scan [TIMEOUT_MS [TOP_N]]");
        return 1;
    }
    if (strcmp(args->argv[1], "scan") != 0) {
        console_reply_err("wifi: unknown subcommand '%s' (try: scan)", args->argv[1]);
        return 1;
    }
    int timeout_ms = args->argc >= 3 ? atoi(args->argv[2]) : 600;
    int top_n      = args->argc >= 4 ? atoi(args->argv[3]) : 8;
    if (top_n < 1)  top_n = 1;
    if (top_n > WIFI_SCAN_MAX_RESULTS) top_n = WIFI_SCAN_MAX_RESULTS;

    wifi_ap_t aps[WIFI_SCAN_MAX_RESULTS];
    int64_t t0 = esp_timer_get_time();
    int n = wifi_scan(aps, top_n, timeout_ms);
    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
    if (n < 0) {
        console_reply_err("wifi_scan failed (n=%d, elapsed=%lldms)", n, elapsed_ms);
        return 1;
    }

    /* Inline-build the JSON array.  Each AP line is conservatively
     * ~90 chars; cap output to avoid blowing the console line. */
    char buf[1400];
    int off = 0;
    off += snprintf(buf + off, sizeof(buf) - off,
                    "{\"count\":%d,\"elapsed_ms\":%lld,\"top\":[", n, elapsed_ms);
    for (int i = 0; i < n && off < (int)sizeof(buf) - 100; ++i) {
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%d,\"auth\":\"%s\"}",
                        i == 0 ? "" : ",",
                        aps[i].ssid,
                        (int)aps[i].rssi,
                        (int)aps[i].channel,
                        wifi_auth_label(aps[i].authmode));
    }
    snprintf(buf + off, sizeof(buf) - off, "]}");
    console_reply_ok("%s", buf);
    return 0;
}

/* ── ?sd ──────────────────────────────────────────────────────────── */

static int cmd_sd(const console_args_t *args)
{
    /* Subcommands let the AI script the same operations the Vault scene
     * exposes via long-press / button cycling. Default (no args) is the
     * status JSON; `?sd remount|bench|format|probe` mirror the GUI. */
    const char *sub = args->argc >= 2 ? args->argv[1] : "";

    if (strcmp(sub, "remount") == 0) {
        bool ok = sdcard_remount();
        sdcard_state_t st;
        sdcard_get(&st);
        console_reply_ok("{\"mounted\":%s,\"err\":\"%s\"}",
                         ok ? "true" : "false",
                         esp_err_to_name(st.last_mount_err));
        return 0;
    }
    if (strcmp(sub, "unmount") == 0) {
        int rc = sdcard_force_unmount();
        console_reply_ok("{\"unmount_rc\":%d}", rc);
        return 0;
    }
    if (strcmp(sub, "bench") == 0) {
        int mb = args->argc >= 3 ? atoi(args->argv[2]) : 1;
        int w_kbps = 0, r_kbps = 0;
        int rc = sdcard_benchmark(mb, &w_kbps, &r_kbps);
        if (rc != 0) {
            console_reply_err("bench rc=%d (mounted=%s)",
                              rc, sdcard_is_mounted() ? "true" : "false");
            return 1;
        }
        console_reply_ok("{\"mb\":%d,\"write_kbps\":%d,\"read_kbps\":%d}",
                         mb, w_kbps, r_kbps);
        return 0;
    }
    if (strcmp(sub, "format") == 0) {
        /* Destructive — require explicit confirm token to avoid
         * fat-fingering from automation. */
        if (args->argc < 3 || strcmp(args->argv[2], "ERASE") != 0) {
            console_reply_err("destructive — use: ?sd format ERASE");
            return 1;
        }
        int rc = sdcard_format();
        if (rc != 0) {
            console_reply_err("format rc=%d", rc);
            return 1;
        }
        sdcard_state_t st;
        sdcard_get(&st);
        console_reply_ok("{\"formatted\":true,\"free_mb\":%llu}",
                         (unsigned long long)(st.free_bytes / (1024ULL * 1024ULL)));
        return 0;
    }
    if (strcmp(sub, "probe") == 0) {
        if (!sdcard_is_mounted()) {
            console_reply_err("no card mounted");
            return 1;
        }
        char path[64];
        snprintf(path, sizeof(path), "%s/aurora_probe.bin", BSP_SD_MOUNT_POINT);
        FILE *f = fopen(path, "wb");
        if (!f) {
            console_reply_err("probe open fail");
            return 1;
        }
        static uint8_t pat[1024];
        for (int i = 0; i < 1024; ++i) pat[i] = (uint8_t)(i ^ 0x5A);
        size_t total = 0;
        for (int i = 0; i < 4; ++i) total += fwrite(pat, 1, sizeof(pat), f);
        fclose(f);
        console_reply_ok("{\"probe_bytes\":%u}", (unsigned)total);
        return 0;
    }

    /* Default: status JSON. */
    sdcard_state_t st;
    sdcard_get(&st);
    if (!st.mounted) {
        console_reply_ok("{\"mounted\":false,\"err\":\"%s\"}",
                         esp_err_to_name(st.last_mount_err));
        return 0;
    }
    char buf[260];
    snprintf(buf, sizeof(buf),
             "{\"mounted\":true,\"name\":\"%s\",\"type\":\"%s\","
             "\"capacity_mb\":%llu,\"used_mb\":%llu,\"free_mb\":%llu,"
             "\"speed_khz\":%d}",
             st.card_name, st.card_type,
             (unsigned long long)(st.capacity_bytes / (1024ULL * 1024ULL)),
             (unsigned long long)(st.used_bytes     / (1024ULL * 1024ULL)),
             (unsigned long long)(st.free_bytes     / (1024ULL * 1024ULL)),
             st.speed_khz);
    console_reply_ok("%s", buf);
    return 0;
}

/* ── audio ────────────────────────────────────────────────────────── */

static int cmd_audio(const console_args_t *args)
{
    if (args->argc < 2) {
        console_reply_err("usage: audio tone FREQ_HZ [DUR_MS [VOL_PCT]]");
        return 1;
    }
    const char *sub = args->argv[1];
    if (strcmp(sub, "tone") == 0) {
        if (args->argc < 3) {
            console_reply_err("usage: audio tone FREQ_HZ [DUR_MS [VOL_PCT]]");
            return 1;
        }
        int freq = atoi(args->argv[2]);
        int dur  = args->argc >= 4 ? atoi(args->argv[3]) : 300;
        int vol  = args->argc >= 5 ? atoi(args->argv[4]) : 60;

        int64_t t0 = esp_timer_get_time();
        int written = audio_play_tone(freq, dur, vol);
        int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
        if (written < 0) {
            console_reply_err("audio_play_tone failed: %d (ready=%s)",
                              written, audio_is_ready() ? "true" : "false");
            return 1;
        }
        console_reply_ok("{\"bytes\":%d,\"freq\":%d,\"requested_ms\":%d,\"elapsed_ms\":%lld,\"vol\":%d}",
                         written, freq, dur, elapsed_ms, vol);
        return 0;
    }
    if (strcmp(sub, "mic") == 0) {
        /* Capture N ms from ES8311 ADC and return peak + RMS in dBFS. */
        int dur = args->argc >= 3 ? atoi(args->argv[2]) : 1000;
        float pk = -100.0f, rms = -100.0f;
        int64_t t0 = esp_timer_get_time();
        int rc = audio_record_peak(dur, &pk, &rms);
        int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
        if (rc != 0) {
            console_reply_err("audio_record_peak: rc=%d (mic ready=%s)",
                              rc, audio_is_ready() ? "true" : "false");
            return 1;
        }
        console_reply_ok("{\"requested_ms\":%d,\"elapsed_ms\":%lld,"
                         "\"peak_dbfs\":%.1f,\"rms_dbfs\":%.1f}",
                         dur, elapsed_ms, pk, rms);
        return 0;
    }
    if (strcmp(sub, "vol") == 0) {
        if (args->argc >= 3) {
            audio_set_volume(atoi(args->argv[2]));
        }
        console_reply_ok("{\"volume\":%d}", audio_get_volume());
        return 0;
    }
    if (strcmp(sub, "boost") == 0) {
        /* Loopback normalisation target peak. 0 = off (raw playback);
         * 28000 ≈ −1.3 dBFS comfortable default. Higher → louder
         * playback but clipping risk if the captured peak is too low. */
        if (args->argc >= 3) {
            audio_set_boost(atoi(args->argv[2]));
        }
        console_reply_ok("{\"boost_target\":%d}", audio_get_boost());
        return 0;
    }
    if (strcmp(sub, "diag") == 0) {
        /* Diagnose "no sound" by reading the PA enable pin's level
         * before / during / after a probe tone, optionally forcing it
         * HIGH ourselves. `audio diag` reads the natural state;
         * `audio diag force` overrides and plays at full volume. */
        bool force = (args->argc >= 3 && strcmp(args->argv[2], "force") == 0);
        audio_diag_t d;
        audio_diag_check_pa(force, &d);
        console_reply_ok("{\"forced_pa\":%s,\"pa_before\":%d,"
                         "\"pa_during\":%d,\"pa_after\":%d,\"tone_rc\":%d}",
                         d.forced_pa ? "true" : "false",
                         d.pa_level_before, d.pa_level_during,
                         d.pa_level_after, d.tone_rc);
        return 0;
    }
    if (strcmp(sub, "loopback") == 0) {
        /* Record + play back inline. Blocking — buffers in PSRAM. */
        int dur = args->argc >= 3 ? atoi(args->argv[2]) : 1000;
        if (dur < 200) dur = 200;
        if (dur > 3000) dur = 3000;
        int n = (int)((int64_t)dur * 22050 / 1000);
        int16_t *buf = (int16_t *)heap_caps_malloc(
            (size_t)n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            console_reply_err("loopback: PSRAM alloc %d samples failed", n);
            return 1;
        }
        float pk = -100.0f, rms = -100.0f;
        int64_t t0 = esp_timer_get_time();
        int rc = audio_record_loopback(buf, n, NULL, NULL, &pk, &rms);
        int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
        heap_caps_free(buf);
        if (rc != 0) {
            console_reply_err("loopback: rc=%d", rc);
            return 1;
        }
        console_reply_ok("{\"requested_ms\":%d,\"elapsed_ms\":%lld,"
                         "\"peak_dbfs\":%.1f,\"rms_dbfs\":%.1f}",
                         dur, elapsed_ms, pk, rms);
        return 0;
    }
    console_reply_err("audio: unknown subcommand '%s' (tone | mic | loopback | vol | boost | diag)", sub);
    return 1;
}

/* ── ?keys ─────────────────────────────────────────────────────────── */

static int cmd_keys(const console_args_t *args)
{
    (void)args;
    keys_state_t k;
    keys_get(&k);
    console_reply_ok("{\"boot\":{\"pressed\":%s,\"count\":%lu},"
                     "\"user\":{\"pressed\":%s,\"count\":%lu},"
                     "\"pwr\":{\"pressed\":%s,\"count\":%lu}}",
                     k.boot_pressed ? "true" : "false", (unsigned long)k.boot_count,
                     k.user_pressed ? "true" : "false", (unsigned long)k.user_count,
                     k.pwr_pressed  ? "true" : "false", (unsigned long)k.pwr_count);
    return 0;
}

/* ── ?power ────────────────────────────────────────────────────────── */

static int cmd_power(const console_args_t *args)
{
    (void)args;
    pmic_state_t st;
    pmic_get(&st);
    char buf[260];
    snprintf(buf, sizeof(buf),
             "{\"ready\":%s,\"vbus\":%s,\"battery\":%s,"
             "\"charge\":\"%s\",\"percent\":%d,\"voltage_mv\":%d,"
             "\"vbus_mv\":%d,\"rate_pct_per_min\":%.2f,"
             "\"rate_mv_per_min\":%.1f}",
             st.ready ? "true" : "false",
             st.vbus_in ? "true" : "false",
             st.battery ? "true" : "false",
             pmic_charge_label(st.charge),
             st.percent,
             st.voltage_mv,
             st.vbus_voltage_mv,
             st.rate_pct_per_min,
             st.rate_mv_per_min);
    console_reply_ok("%s", buf);
    return 0;
}

/* ── ?ota ─────────────────────────────────────────────────────────── */

static const char *ota_state_label(esp_ota_img_states_t s)
{
    switch (s) {
        case ESP_OTA_IMG_NEW:            return "NEW";
        case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
        case ESP_OTA_IMG_VALID:          return "VALID";
        case ESP_OTA_IMG_INVALID:        return "INVALID";
        case ESP_OTA_IMG_ABORTED:        return "ABORTED";
        case ESP_OTA_IMG_UNDEFINED:      return "UNDEFINED";
        default:                          return "?";
    }
}

static int cmd_ota(const console_args_t *args)
{
    /* Subcommand: info (default) | mark-valid | rollback */
    const char *sub = (args->argc >= 2) ? args->argv[1] : "info";

    if (strcmp(sub, "mark-valid") == 0) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            console_reply_err("mark-valid: %s", esp_err_to_name(err));
            return 1;
        }
        console_reply_ok("{\"action\":\"mark-valid\",\"result\":\"ok\"}");
        return 0;
    }
    if (strcmp(sub, "rollback") == 0) {
        /* esp_ota_mark_app_invalid_rollback_and_reboot reboots immediately. */
        console_reply_ok("{\"action\":\"rollback\",\"note\":\"rebooting\"}");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
        /* If we return at all, rollback failed. */
        console_reply_err("rollback failed: %s", esp_err_to_name(err));
        return 1;
    }
    if (strcmp(sub, "info") != 0) {
        console_reply_err("ota: unknown subcmd '%s' (info|mark-valid|rollback)", sub);
        return 1;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot    = esp_ota_get_boot_partition();
    const esp_partition_t *next    = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running) esp_ota_get_state_partition(running, &state);

    bool rollback_possible = false;
    const esp_partition_t *last_invalid = esp_ota_get_last_invalid_partition();
    if (state == ESP_OTA_IMG_PENDING_VERIFY ||
        (state == ESP_OTA_IMG_VALID && last_invalid != NULL)) {
        rollback_possible = true;
    }

    char buf[420];
    snprintf(buf, sizeof(buf),
             "{\"running\":{\"label\":\"%s\",\"address\":\"0x%08lx\",\"size\":%lu,\"state\":\"%s\"},"
             "\"boot\":{\"label\":\"%s\"},"
             "\"next_update\":{\"label\":\"%s\"},"
             "\"last_invalid\":{\"label\":\"%s\"},"
             "\"rollback_possible\":%s}",
             running ? running->label : "?",
             running ? (unsigned long)running->address : 0UL,
             running ? (unsigned long)running->size : 0UL,
             ota_state_label(state),
             boot ? boot->label : "?",
             next ? next->label : "?",
             last_invalid ? last_invalid->label : "none",
             rollback_possible ? "true" : "false");
    console_reply_ok("%s", buf);
    return 0;
}

static const console_cmd_t s_cmd_ota   = { "?ota",    cmd_ota,
    "?ota [info|mark-valid|rollback]: OTA partition state + rollback control" };

/* ?stat / scene / tap / swipe / ?dump all registered via
 * harness_default_register() in aurora-harness. */
static const console_cmd_t s_cmd_sensor = { "?sensor", cmd_sensor,
    "JSON: accel (g, screen frame) + ready flag" };
static const console_cmd_t s_cmd_power  = { "?power",  cmd_power,
    "JSON: battery percent / voltage / charge state / vbus" };
static const console_cmd_t s_cmd_audio  = { "audio",   cmd_audio,
    "audio tone|mic|loopback|vol: speaker tone / mic peak / record+play / volume" };
static const console_cmd_t s_cmd_keys   = { "?keys",   cmd_keys,
    "JSON: BOOT / USER / PWR button state + press counters" };
static const console_cmd_t s_cmd_sys    = { "?sys",    cmd_sys,
    "JSON: CPU / heap / temp / MAC / IDF / reset reason / uptime" };
static const console_cmd_t s_cmd_sd     = { "?sd",     cmd_sd,
    "?sd [remount|bench MB|probe|format ERASE]: SD status + mgmt" };
static const console_cmd_t s_cmd_wifi   = { "wifi",    cmd_wifi,
    "wifi scan [TIMEOUT_MS [TOP_N]]: STA scan, JSON top-N by RSSI" };
static const console_cmd_t s_cmd_ble    = { "ble",     cmd_ble,
    "ble scan [DUR_MS [MAX_N]]: passive observer, JSON unique devices" };
static const console_cmd_t s_cmd_radio  = { "radio",   cmd_radio,
    "radio [ble|wifi]: toggle which radio owns the internal-SRAM pool" };

void harness_commands_register(void)
{
    /* Pull in the default set (?stat today) from aurora-harness. */
    harness_default_register();
    /* ?dump registered by harness_default_register() in aurora-harness */
    /* tap / swipe registered by harness_default_register() in aurora-harness */
    /* scene cmd registered by harness_default_register() in aurora-harness */
    console_protocol_register(&s_cmd_sensor);
    console_protocol_register(&s_cmd_power);
    console_protocol_register(&s_cmd_audio);
    console_protocol_register(&s_cmd_sd);
    console_protocol_register(&s_cmd_wifi);
    console_protocol_register(&s_cmd_ble);
    console_protocol_register(&s_cmd_radio);
    console_protocol_register(&s_cmd_keys);
    console_protocol_register(&s_cmd_sys);
    console_protocol_register(&s_cmd_ota);
    ESP_LOGI(TAG, "harness commands registered (15)");
}
