/*
 * screenshot.c — `?dump` console command implementation.
 *
 * See harness/screenshot.h for the user-facing contract. Implementation
 * notes — read carefully if you're modifying:
 *
 *   - The screen-sized PSRAM draw buffer is allocated ONCE (lazy on
 *     first call) and reused. Allocating per-dump from the default heap
 *     wedged the device on the first iteration; pinning to PSRAM with a
 *     low-water-mark check fixed it.
 *   - The snapshot itself runs on the LVGL task via lv_async_call. The
 *     LVGL task implicitly owns the LVGL mutex during its callbacks,
 *     so the snapshot routine doesn't need to grab anything. The
 *     console task (which received the `?dump` line) waits on a
 *     semaphore up to 3 s.
 *   - We snapshot the screen + lv_layer_top() separately and composite
 *     the top in software. lv_snapshot_take on the active screen alone
 *     misses widgets parented to lv_layer_top, which is where the
 *     UI shell (dots / scene name) lives in Aurora.
 *   - The downsample is a box filter that averages in 8-bit channel
 *     space after RGB565 bit-replication-expand. Nearest-neighbour
 *     downsamples produce "rainbow speckles" on anti-aliased thin lines
 *     because the RGB565 quantisation grid doesn't preserve hue across
 *     neighbouring AA pixels.
 */

#include "harness/screenshot.h"
#include "harness/console_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* ── base64 ──────────────────────────────────────────────────────── */

static const char b64chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode_chunk(const uint8_t *in, size_t n, char *out)
{
    /* n must be 1, 2, or 3 */
    uint32_t v = (uint32_t)in[0] << 16;
    if (n > 1) v |= (uint32_t)in[1] << 8;
    if (n > 2) v |= (uint32_t)in[2];
    out[0] = b64chars[(v >> 18) & 0x3F];
    out[1] = b64chars[(v >> 12) & 0x3F];
    out[2] = (n > 1) ? b64chars[(v >> 6) & 0x3F] : '=';
    out[3] = (n > 2) ? b64chars[v & 0x3F]        : '=';
}

/* ── persistent snapshot buffer + async machinery ─────────────────── */

static lv_draw_buf_t      *s_snap_buf = NULL;
static int32_t             s_snap_w   = 0;
static int32_t             s_snap_h   = 0;
static SemaphoreHandle_t   s_dump_done_sem;
static lv_result_t         s_dump_async_result;

/* Composite a top-layer ARGB8888 buffer over a screen RGB565 buffer.
 * In-place on screen_buf. */
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

static void do_snapshot_async(void *user)
{
    (void)user;
    s_dump_async_result = lv_snapshot_take_to_draw_buf(
        lv_screen_active(), LV_COLOR_FORMAT_RGB565, s_snap_buf);

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

/* ── cmd_dump ────────────────────────────────────────────────────── */

/* Maximum target_w we accept. Bumped from the legacy 256 cap so the
 * agent-dashboard's dual-pane sessions scene can be captured at full
 * panel resolution (gap G-F1b). At 1024×1024×2 RGB565 the downsample
 * buffer is ~2 MB, still well inside the typical PSRAM budget (the
 * Aurora board has 8 MB external + 512 KB internal) — but the firmware
 * caps at the active screen dimensions anyway, so a `w=2048` request
 * downgrades to whatever the panel actually is. */
#define DUMP_TARGET_W_MIN  32
#define DUMP_TARGET_W_MAX  2048

static int cmd_dump(const console_args_t *args)
{
    /* requested = what the host asked for (before clamping); actual =
     * what the device will emit. Both end up in the OK line so the host
     * can detect silent downgrades (G-F1b). */
    int requested_w = 128;
    bool w_was_set = false;
    for (int i = 1; i < args->argc; ++i) {
        const char *a = args->argv[i];
        if (strncmp(a, "w=", 2) == 0) {
            int v = atoi(a + 2);
            if (v > 0) {
                requested_w = v;
                w_was_set = true;
            }
        }
    }
    /* Clamp to the absolute legal range first. */
    int target_w = requested_w;
    const char *clamp_reason = "ok";
    if (!w_was_set) {
        clamp_reason = "default";
    } else if (target_w < DUMP_TARGET_W_MIN) {
        target_w = DUMP_TARGET_W_MIN;
        clamp_reason = "below_min";
    } else if (target_w > DUMP_TARGET_W_MAX) {
        target_w = DUMP_TARGET_W_MAX;
        clamp_reason = "above_max";
    }

    lv_obj_t *scr = lv_screen_active();
    int32_t src_w = lv_obj_get_width(scr);
    int32_t src_h = lv_obj_get_height(scr);
    if (src_w <= 0 || src_h <= 0) {
        console_reply_err("invalid screen size %dx%d", (int)src_w, (int)src_h);
        return 1;
    }
    /* And clamp to the active screen — there is no useful information
     * past panel resolution and we'd waste PSRAM on the downsample
     * buffer. The host learns from `reason=panel_cap` in the OK line
     * that it didn't get the size it asked for. */
    if (target_w > src_w) {
        target_w = (int)src_w;
        clamp_reason = "panel_cap";
    }
    int target_h = target_w;

    if (s_snap_buf == NULL || s_snap_w != src_w || s_snap_h != src_h) {
        if (s_snap_buf) {
            lv_draw_buf_destroy(s_snap_buf);
            s_snap_buf = NULL;
        }
        size_t need = (size_t)src_w * (size_t)src_h * 2;
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

    if (s_dump_done_sem == NULL) s_dump_done_sem = xSemaphoreCreateBinary();
    xSemaphoreTake(s_dump_done_sem, 0);

    lv_async_call(do_snapshot_async, NULL);

    if (xSemaphoreTake(s_dump_done_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
        console_reply_err("snapshot timed out (LVGL task not draining)");
        return 1;
    }
    if (s_dump_async_result != LV_RESULT_OK) {
        console_reply_err("snapshot failed: %d", (int)s_dump_async_result);
        return 1;
    }
    lv_draw_buf_t *snap = s_snap_buf;

    /* Box-filter downsample to target_w x target_h in PSRAM. */
    size_t out_px = (size_t)target_w * (size_t)target_h;
    size_t out_bytes = out_px * 2;
    uint16_t *small = (uint16_t *)heap_caps_malloc(out_bytes,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!small) {
        console_reply_err("psram alloc %u bytes failed", (unsigned)out_bytes);
        return 1;
    }

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

    char meta[160];
    snprintf(meta, sizeof(meta), "w=%d h=%d fmt=RGB565LE bytes=%u",
             target_w, target_h, (unsigned)out_bytes);
    /* Self-describing OK: name the tag so host parsers don't have to
     * grep firmware source (agent-dashboard G-4), and include the
     * requested-vs-actual width + reason so the host can detect
     * silent downgrades from the requested w (G-F1b). */
    console_reply_ok(
        "dump start tag=DUMP %s w_requested=%d w_actual=%d reason=%s",
        meta, requested_w, target_w, clamp_reason);
    console_begin_payload("DUMP", meta);

    /* 48 input bytes per 64-char output line. */
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

static const console_cmd_t s_cmd_dump = { "?dump", cmd_dump,
    "screenshot DUMP_BEGIN/END base64 RGB565 (default 128x128; pass w=N; "
    "capped at panel width or 2048 — see w_actual/reason in OK line)" };

void harness_screenshot_register(void)
{
    console_protocol_register(&s_cmd_dump);
}
