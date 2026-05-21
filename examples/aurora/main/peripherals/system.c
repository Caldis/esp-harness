/*
 * system.c — pull together a system / SoC snapshot.
 *
 * Initial design is read-only and lock-free. The "moving" fields (heap,
 * temp, uptime) are re-read on every system_get(); the "static"
 * identifier strings (MACs, IDF version, etc.) are computed once at
 * system_init and reused. Total cost of a get is one temp_sensor_read
 * + a few heap_caps_get_* — well under 1 ms.
 */

#include "system.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
/* esp_flash_get_size is in esp_flash.h — but that pulls SPI flash
 * private headers we don't need. Use the spi_flash_chip API instead. */
#include "esp_flash.h"
#include "driver/temperature_sensor.h"

static const char *TAG = "sys";

static system_info_t s_info;
static temperature_sensor_handle_t s_temp_handle = NULL;

static const char *reset_reason_to_string(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_UNKNOWN:    return "UNKNOWN";
        case ESP_RST_POWERON:    return "POWERON";
        case ESP_RST_EXT:        return "EXT_PIN";
        case ESP_RST_SW:         return "SW_RESET";
        case ESP_RST_PANIC:      return "PANIC";
        case ESP_RST_INT_WDT:    return "INT_WDT";
        case ESP_RST_TASK_WDT:   return "TASK_WDT";
        case ESP_RST_WDT:        return "WDT_OTHER";
        case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:   return "BROWNOUT";
        case ESP_RST_SDIO:       return "SDIO";
        default:                  return "?";
    }
}

static void format_mac(uint8_t mac[6], char *out, size_t cap)
{
    snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool system_init(void)
{
    memset(&s_info, 0, sizeof(s_info));

    /* ---- Chip + clock ---- */
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);
    /* `revision` is "major*100 + minor" in IDF >=5.0. The older
     * `full_revision` name was deprecated.  */
    s_info.chip_revision = chip.revision;
    s_info.chip_cores = chip.cores;
    s_info.xtal_mhz = 40;   /* ESP32-S3 always 40 MHz */
    /* CPU freq refreshed every get; here just populate once. */

    /* ---- Flash ---- */
    uint32_t flash_bytes = 0;
    esp_flash_get_size(NULL, &flash_bytes);
    s_info.flash_size_mb = flash_bytes / (1024U * 1024U);

    /* ---- MACs from efuse ---- */
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        format_mac(mac, s_info.wifi_mac, sizeof(s_info.wifi_mac));
    }
    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
        format_mac(mac, s_info.bt_mac, sizeof(s_info.bt_mac));
    }

    /* ---- Versions ---- */
    snprintf(s_info.idf_version, sizeof(s_info.idf_version), "%s", IDF_VER);
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        strlcpy(s_info.app_name, desc->project_name, sizeof(s_info.app_name));
        strlcpy(s_info.app_version, desc->version, sizeof(s_info.app_version));
        snprintf(s_info.build_time, sizeof(s_info.build_time),
                 "%s %s", desc->date, desc->time);
        /* First 14 hex chars of the SHA — readable, still unique enough
         * for cross-version checks. */
        for (int i = 0; i < 7 && i * 2 + 1 < (int)sizeof(s_info.elf_sha256_short) - 1; ++i) {
            snprintf(&s_info.elf_sha256_short[i * 2], 3, "%02x",
                     desc->app_elf_sha256[i]);
        }
        s_info.elf_sha256_short[14] = '\0';
    }

    /* ---- Reset reason ---- */
    strlcpy(s_info.reset_reason, reset_reason_to_string(esp_reset_reason()),
            sizeof(s_info.reset_reason));

    /* ---- SoC internal temperature sensor ---- */
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 80);
    if (temperature_sensor_install(&cfg, &s_temp_handle) == ESP_OK) {
        temperature_sensor_enable(s_temp_handle);
    } else {
        ESP_LOGW(TAG, "temperature_sensor_install failed");
        s_temp_handle = NULL;
    }

    s_info.static_inited = true;
    ESP_LOGI(TAG, "system_init: %s rev v%d.%d, %s, %lu KB flash",
             s_info.app_name, s_info.chip_revision / 100,
             s_info.chip_revision % 100,
             s_info.reset_reason,
             (unsigned long)(s_info.flash_size_mb * 1024));
    return true;
}

void system_get(system_info_t *out)
{
    if (!out) return;
    if (!s_info.static_inited) {
        /* Caller forgot to init — return zeros rather than crash. */
        memset(out, 0, sizeof(*out));
        return;
    }

    /* CPU freq — esp_clk_cpu_freq isn't exposed cleanly across IDF
     * versions; the kHz number comes back through the chip clock API.
     * For ESP32-S3 we hardcode the configured boot frequency from
     * Kconfig (240 MHz). PM if added later can override. */
    s_info.cpu_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;

    /* Memory caps. INTERNAL = DRAM (SRAM), SPIRAM = PSRAM. */
    s_info.heap_internal_free    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_info.heap_internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    s_info.heap_internal_min     = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    s_info.heap_psram_free       = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s_info.heap_psram_largest    = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    s_info.heap_psram_min        = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

    /* SoC temperature. The driver takes ~10 µs per read. */
    float tc = 0.0f;
    if (s_temp_handle != NULL &&
        temperature_sensor_get_celsius(s_temp_handle, &tc) == ESP_OK) {
        s_info.soc_temp_c = tc;
    }

    s_info.uptime_ms = (uint64_t)(esp_timer_get_time() / 1000);

    memcpy(out, &s_info, sizeof(*out));
}
