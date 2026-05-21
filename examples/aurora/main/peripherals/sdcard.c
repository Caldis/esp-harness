/*
 * sdcard.c — mount + hot-plug + benchmark + format.
 *
 * The Waveshare BSP exposes:
 *   - bsp_sdcard_mount()  — runs esp_vfs_fat_sdmmc_mount under the
 *                           hood, then leaves the global `bsp_sdcard`
 *                           pointer non-NULL. Returns ESP_OK / fail.
 *   - bsp_sdcard          — sdmmc_card_t* once mounted.
 * It does NOT expose unmount. We use esp_vfs_fat_sdcard_unmount
 * directly when we need to drop and remount.
 *
 * Hot-plug strategy: no card-detect GPIO on this board, so we can
 * only know if a card is present by attempting to init it. The scene
 * calls `sdcard_remount` on entry and on long press, which is the
 * only way to recover from a "user just inserted a card" event.
 */

#include "sdcard.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>      /* fsync() */
#include <sys/statvfs.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "bsp/esp-bsp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "sdcard";

static sdcard_state_t s_state = { .mounted = false };

/* Polling task control. s_poll_enabled is read every wake of the
 * background task; transitions are signalled via s_poll_signal so we
 * can wake immediately on enable instead of waiting out a 2 s tick. */
static volatile bool   s_poll_enabled = false;
static SemaphoreHandle_t s_poll_signal = NULL;
static TaskHandle_t    s_poll_task = NULL;

/* Forward declarations of internals we need below. */
static void refresh_inplace(void);

static const char *type_name(const sdmmc_card_t *c)
{
    if (c == NULL) return "?";
    if (c->is_mmc) return "MMC";
    if (c->ocr & (1u << 30)) {
        /* CCS bit set → high-capacity card. > 2 GB is SDHC, > 32 GB is SDXC. */
        if (c->csd.capacity > (32ULL * 1024 * 1024 * 1024 / 512)) return "SDXC";
        return "SDHC";
    }
    return "SDSC";
}

static void populate_state_from_bsp(void)
{
    if (bsp_sdcard == NULL) return;
    uint64_t blocks = (uint64_t)bsp_sdcard->csd.capacity;
    uint64_t bsize  = (uint64_t)bsp_sdcard->csd.sector_size;
    s_state.capacity_bytes = blocks * bsize;
    s_state.speed_khz = bsp_sdcard->max_freq_khz;
    strlcpy(s_state.card_name, bsp_sdcard->cid.name, sizeof(s_state.card_name));
    strlcpy(s_state.card_type, type_name(bsp_sdcard), sizeof(s_state.card_type));
    refresh_inplace();
}

static void refresh_inplace(void)
{
    if (!s_state.mounted) {
        s_state.free_bytes = 0;
        s_state.used_bytes = 0;
        return;
    }
    struct statvfs vfs;
    if (statvfs(BSP_SD_MOUNT_POINT, &vfs) == 0) {
        s_state.free_bytes = (uint64_t)vfs.f_bsize * (uint64_t)vfs.f_bfree;
    } else {
        s_state.free_bytes = 0;
    }
    s_state.used_bytes = (s_state.capacity_bytes > s_state.free_bytes)
                       ? s_state.capacity_bytes - s_state.free_bytes
                       : 0;
}

/* Background polling task. Sleeps on s_poll_signal until something
 * pokes it (enable / shutdown / external request), then loops at 2 s
 * intervals while polling is enabled AND the card isn't yet mounted.
 * Auto-parks once mounted so a happy card costs literally nothing. */
static void sdcard_poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_poll_enabled && !s_state.mounted) {
            sdcard_remount();   /* harmless if no card — returns false */
            /* Block up to 2 s; an enable/disable signal wakes us early. */
            xSemaphoreTake(s_poll_signal, pdMS_TO_TICKS(2000));
        } else {
            /* Park indefinitely until s_poll_signal is given. */
            xSemaphoreTake(s_poll_signal, portMAX_DELAY);
        }
    }
}

void sdcard_polling_enable(bool enable)
{
    s_poll_enabled = enable;
    if (s_poll_signal) xSemaphoreGive(s_poll_signal);
}

bool sdcard_init(void)
{
    /* Background poller used by Vault for hot-plug detection. */
    if (s_poll_signal == NULL) {
        s_poll_signal = xSemaphoreCreateBinary();
    }
    if (s_poll_task == NULL && s_poll_signal != NULL) {
        /* Low priority — UI must always win. 3 KB stack fits the SDIO
         * call chain comfortably. */
        BaseType_t ok = xTaskCreate(sdcard_poll_task, "sd_poll",
                                    3072, NULL, 2, &s_poll_task);
        if (ok != pdPASS) {
            ESP_LOGW(TAG, "sd_poll task create failed");
        }
    }

    /* Initial mount attempt — on the LVGL task is fine here because
     * we're still in app_main before scenes are running. Subsequent
     * retries go through the background task. */
    return sdcard_remount();
}

bool sdcard_remount(void)
{
    if (s_state.mounted) {
        refresh_inplace();
        s_state.last_mount_err = 0;
        return true;
    }

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = bsp_sdcard_mount();
    int el_ms = (int)((esp_timer_get_time() - t0) / 1000);
    int prev_err = s_state.last_mount_err;
    s_state.last_mount_err = (int)err;
    /* DEBUG: surface how long a failed mount actually blocks. If this
     * routinely exceeds ~100 ms we should move polling off the LVGL
     * task into a background thread. */
    ESP_LOGI(TAG, "remount attempt: %s in %d ms",
             esp_err_to_name(err), el_ms);

    if (err == ESP_ERR_INVALID_STATE && bsp_sdcard != NULL) {
        /* BSP says "already mounted" — treat as success. */
        s_state.mounted = true;
        populate_state_from_bsp();
        return true;
    }
    if (err != ESP_OK) {
        /* Card-detect polling path runs this every 2 s when no card
         * is in. Suppress repeated warnings about the same failure;
         * log only when the error *changes* (e.g., timeout → fs error
         * gives a clue something changed in the slot). */
        if (err != prev_err) {
            ESP_LOGW(TAG, "mount: %s (insert card or check filesystem)",
                     esp_err_to_name(err));
        }
        s_state.mounted = false;
        return false;
    }
    if (bsp_sdcard == NULL) {
        ESP_LOGW(TAG, "mount returned OK but bsp_sdcard is NULL");
        s_state.mounted = false;
        return false;
    }
    s_state.mounted = true;
    populate_state_from_bsp();
    ESP_LOGI(TAG, "mounted '%s' (%s)  %lluMB / %lluMB free  %d kHz",
             s_state.card_name, s_state.card_type,
             (unsigned long long)(s_state.capacity_bytes / (1024ULL * 1024ULL)),
             (unsigned long long)(s_state.free_bytes / (1024ULL * 1024ULL)),
             s_state.speed_khz);
    return true;
}

bool sdcard_is_mounted(void) { return s_state.mounted; }

void sdcard_get(sdcard_state_t *out)
{
    if (!out) return;
    if (s_state.mounted) refresh_inplace();
    memcpy(out, &s_state, sizeof(*out));
}

void sdcard_refresh(void)
{
    if (s_state.mounted) refresh_inplace();
}

/* ── benchmark ────────────────────────────────────────────────────── */

int sdcard_benchmark(int mb, int *out_write_kbps, int *out_read_kbps)
{
    if (out_write_kbps) *out_write_kbps = 0;
    if (out_read_kbps)  *out_read_kbps  = 0;
    if (!s_state.mounted) return -1;
    if (mb < 1)  mb = 1;
    if (mb > 16) mb = 16;   /* keep test bounded — 16 MB is enough to
                             * average over SD-card internal write
                             * caching while not eating user space. */

    const int CHUNK = 4096;
    const int total_bytes = mb * 1024 * 1024;
    const int chunks = total_bytes / CHUNK;

    uint8_t *buf = (uint8_t *)malloc(CHUNK);
    if (!buf) return -1;
    /* Fill with a recognisable pattern (also exercises any SD-side
     * data compression / dedup defences — they shouldn't optimise
     * this away). */
    for (int i = 0; i < CHUNK; ++i) buf[i] = (uint8_t)(i ^ 0xA5);

    char path[64];
    snprintf(path, sizeof(path), "%s/aurora_bench.bin", BSP_SD_MOUNT_POINT);

    /* ---- write phase ---- */
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "bench: fopen wb failed (%s)", path);
        free(buf);
        return -2;
    }
    int64_t t0 = esp_timer_get_time();
    size_t total_written = 0;
    for (int i = 0; i < chunks; ++i) {
        size_t w = fwrite(buf, 1, CHUNK, f);
        if (w != (size_t)CHUNK) {
            ESP_LOGE(TAG, "bench: short write at chunk %d (%zu/%d)", i, w, CHUNK);
            fclose(f);
            free(buf);
            return -3;
        }
        total_written += w;
    }
    fflush(f);
    fsync(fileno(f));
    int64_t write_us = esp_timer_get_time() - t0;
    fclose(f);

    /* Speed: KB/s = (bytes/1024) / (us/1e6) = bytes * 1e6 / 1024 / us */
    if (out_write_kbps && write_us > 0) {
        *out_write_kbps = (int)((int64_t)total_written * 1000LL * 1000LL
                                 / 1024LL / write_us);
    }

    /* ---- read phase ---- */
    f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "bench: fopen rb failed");
        free(buf);
        return -4;
    }
    t0 = esp_timer_get_time();
    size_t total_read = 0;
    for (int i = 0; i < chunks; ++i) {
        size_t r = fread(buf, 1, CHUNK, f);
        if (r != (size_t)CHUNK) {
            ESP_LOGE(TAG, "bench: short read at chunk %d (%zu/%d)", i, r, CHUNK);
            fclose(f);
            free(buf);
            return -5;
        }
        total_read += r;
    }
    int64_t read_us = esp_timer_get_time() - t0;
    fclose(f);
    free(buf);

    if (out_read_kbps && read_us > 0) {
        *out_read_kbps = (int)((int64_t)total_read * 1000LL * 1000LL
                                / 1024LL / read_us);
    }

    /* Update free space — we just wrote MB. */
    refresh_inplace();

    ESP_LOGI(TAG, "bench %d MB: write=%d KB/s (%.1f MB/s)  read=%d KB/s (%.1f MB/s)",
             mb,
             out_write_kbps ? *out_write_kbps : 0,
             out_write_kbps ? *out_write_kbps / 1024.0f : 0.0f,
             out_read_kbps ? *out_read_kbps : 0,
             out_read_kbps ? *out_read_kbps / 1024.0f : 0.0f);
    return 0;
}

/* ── format ────────────────────────────────────────────────────────── */

int sdcard_force_unmount(void)
{
    if (!s_state.mounted) return 0;
    /* BSP doesn't expose its unmount in the public header — call the
     * underlying ESP-IDF helper directly. */
    esp_err_t err = esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, bsp_sdcard);
    s_state.mounted = false;
    s_state.last_mount_err = (int)err;
    ESP_LOGI(TAG, "force unmount: %s", esp_err_to_name(err));
    return (int)err;
}

int sdcard_format(void)
{
    if (!s_state.mounted) {
        /* Format on an unmounted card needs a mount-style init first,
         * so we don't even try here — surface a clear error. */
        return -1;
    }
    /* esp_vfs_fat_sdcard_format keeps the card mounted afterwards. */
    esp_err_t err = esp_vfs_fat_sdcard_format(BSP_SD_MOUNT_POINT, bsp_sdcard);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "format: %s", esp_err_to_name(err));
        return (int)err;
    }
    refresh_inplace();
    ESP_LOGI(TAG, "format ok — %llu MB free",
             (unsigned long long)(s_state.free_bytes / (1024ULL * 1024ULL)));
    return 0;
}
