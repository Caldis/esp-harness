/*
 * system.h — read-only system / SoC introspection.
 *
 * Sources of truth pulled together for the System scene + `?sys`
 * console command:
 *   - CPU frequency (esp_clk_cpu_freq / esp_pm if dynamic scaling)
 *   - Internal + PSRAM heap stats
 *   - Internal temperature sensor (ESP32-S3 SoC has one)
 *   - Reset reason (POWERON, WDT, BROWNOUT, …) translated to text
 *   - WiFi + BT MAC addresses (efuse)
 *   - Chip revision, IDF version, project name, build time, ELF SHA
 *   - Flash size + partition count
 *
 * Everything is cheap to query; the System scene refreshes the moving
 * fields (heap, temp, fps, uptime) at 1 Hz.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* CPU */
    int   cpu_freq_mhz;
    int   xtal_mhz;
    int   chip_revision;   /* e.g. 2 = v0.2 */
    int   chip_cores;

    /* Memory */
    uint32_t  heap_internal_free;
    uint32_t  heap_internal_largest;
    uint32_t  heap_internal_min;
    uint32_t  heap_psram_free;
    uint32_t  heap_psram_largest;
    uint32_t  heap_psram_min;
    uint32_t  flash_size_mb;

    /* Thermal */
    float  soc_temp_c;     /* ESP32-S3 internal sensor */

    /* Liveness */
    uint64_t uptime_ms;

    /* Identifiers (static, set once at init) */
    char  wifi_mac[18];      /* "aa:bb:cc:dd:ee:ff" */
    char  bt_mac[18];
    char  idf_version[24];
    char  reset_reason[24];  /* "POWERON" / "WDT" / "BROWNOUT" / ... */
    char  app_name[32];
    char  app_version[32];
    char  build_time[40];
    char  elf_sha256_short[16];   /* first 14 hex chars */

    /* Sleep / power */
    bool  static_inited;     /* false until system_init populated the
                              * identifier strings */
} system_info_t;

bool system_init(void);

/* Refresh the moving fields (heap, temp, uptime, cpu freq). Identifiers
 * stay cached. Cheap (~50 µs); safe from any task. */
void system_get(system_info_t *out);

#ifdef __cplusplus
}
#endif
