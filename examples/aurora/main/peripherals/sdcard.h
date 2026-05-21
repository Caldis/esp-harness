/*
 * sdcard.h — microSD card management.
 *
 * Mount, benchmark, and format the FAT-formatted SD card on the
 * board's SDIO slot (BSP_SD_D0/CLK/CMD = GPIO 3/2/1). All operations
 * are safe to call from any task; the underlying BSP / FATFS path
 * serialises internally.
 *
 * Hot-plug: there is NO card-detect GPIO wired on this board, so we
 * can't fire an event on insert/eject. Instead, the Vault scene
 * re-tries `sdcard_remount` on entry and on demand (long press) — the
 * card-init handshake is what tells us whether something is in the
 * slot.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool      mounted;
    uint64_t  capacity_bytes;  /* total card capacity */
    uint64_t  free_bytes;      /* free space (FATFS) */
    uint64_t  used_bytes;      /* capacity − free; reported separately to
                                 * avoid every caller re-computing */
    char      card_name[16];   /* SDcard CID name */
    char      card_type[12];   /* "SDSC" / "SDHC" / "SDXC" / "MMC" */
    int       speed_khz;       /* bus clock */
    int       last_mount_err;  /* esp_err_t from the last mount attempt;
                                 * 0 if mounted, non-zero is informational
                                 * (no card / fs corrupted / hw fault). */
} sdcard_state_t;

bool sdcard_init(void);
bool sdcard_is_mounted(void);

/* Background hot-plug polling. The SD-card slot has no card-detect
 * GPIO on this board, so the only way to notice an inserted card is to
 * retry mount. The mount call BLOCKS while the SDIO driver waits for
 * a response (~100-300 ms on timeout) — way too long to do from the
 * LVGL task. Instead, sdcard.c owns a low-priority background task
 * that polls every 2 s while polling is enabled.
 *
 * Scenes that care about hot-plug (just Vault today) call
 * `sdcard_polling_enable(true)` in their on_show and
 * `sdcard_polling_enable(false)` in their on_hide. The task itself
 * stays parked when disabled — no CPU, no SDIO traffic, no log noise.
 *
 * Once a card is successfully mounted the task auto-pauses until
 * polling is re-enabled (typical use: another card-eject event would
 * need polling re-armed; for now we leave it user-triggered via
 * remount). */
void sdcard_polling_enable(bool enable);

/* Force-unmount (testing aid): drop the FATFS volume + reset internal
 * state so the next remount goes through the full mount path. Useful
 * for measuring no-card mount-fail timing without physically pulling
 * the card. Returns 0 on success. */
int sdcard_force_unmount(void);

/* Cached snapshot — refreshed on remount + by background `sdcard_refresh`
 * when you want fresh free-space numbers. Safe lock-free reader. */
void sdcard_get(sdcard_state_t *out);

/* Cheap operation — re-reads statvfs only. Doesn't touch hardware. */
void sdcard_refresh(void);

/* Try (re)mounting. If a card is already mounted, this is a no-op
 * returning true. If no card is present or mount fails, the state's
 * `last_mount_err` field carries the esp_err_t and the function
 * returns false. Use this for hot-plug. */
bool sdcard_remount(void);

/* Write `mb` megabytes of a known pattern, fsync, then read it back
 * and verify. Returns 0 on success or negative esp_err_t. On success,
 * fills the speed pointers with KB/s for the write and read phases.
 * Files are created in /sdcard/aurora_bench_<size>.bin and left in
 * place — they're idempotent across runs. */
int  sdcard_benchmark(int mb, int *out_write_kbps, int *out_read_kbps);

/* Format the card to FAT (destructive!). Unmounts, formats, remounts.
 * Returns 0 on success. The caller is responsible for confirming the
 * user actually wants this. */
int  sdcard_format(void);

#ifdef __cplusplus
}
#endif
