idf_component_register(
    SRCS
        "aurora_main.c"
        "ui_shell.c"
        # console_protocol + scene_framework moved to
        # components/aurora-harness/ in v1.2 — pulled in via REQUIRES.
        # harness_commands.c stays here as Aurora-specific peripheral glue.
        "harness/harness_commands.c"
        "scenes/scene_halo.c"
        "scenes/scene_grid.c"
        "scenes/scene_bloom.c"
        "scenes/scene_tilt.c"
        "scenes/scene_pulse.c"
        "scenes/scene_echo.c"
        "scenes/scene_vault.c"
        "scenes/scene_whisper.c"
        "scenes/scene_spectrum.c"
        "scenes/scene_cell.c"
        "scenes/scene_keys.c"
        "scenes/scene_listen.c"
        "scenes/scene_tone.c"
        "scenes/scene_system.c"
        "scenes/scene_glow.c"
        "scenes/scene_spin.c"
        "scenes/scene_survey.c"
        "scenes/scene_sniff.c"
        "scenes/scene_notify.c"
        "scenes/scene_track.c"
        "peripherals/imu.c"
        "peripherals/pmic.c"
        "peripherals/audio.c"
        "peripherals/keys.c"
        "peripherals/sdcard.c"
        "peripherals/wifi.c"
        "peripherals/wifi_creds.c"
        "peripherals/ble.c"
        "peripherals/system.c"
        "peripherals/settings.c"
    INCLUDE_DIRS
        "."
        "harness"
        "scenes"
        "peripherals"
    REQUIRES
        # Public headers in main consume LVGL types (e.g. scene_t holds
        # lv_obj_t*), so lvgl needs to be in REQUIRES not PRIV_REQUIRES.
        lvgl__lvgl
        esp32_s3_touch_amoled_2_16
        # Reusable scaffold layer: line protocol + scene framework.
        # See components/aurora-harness/README.md.
        aurora-harness
        # esp_driver_i2c here is upstream-bug workaround: waveshare/qmi8658
        # only REQUIRES "driver" (the v5-era umbrella) but v6 split out
        # i2c_master into esp_driver_i2c. Forcing it visible main-wide
        # keeps the qmi8658 component buildable without patching upstream.
        esp_driver_i2c
    PRIV_REQUIRES
        esp_timer
        esp_driver_usb_serial_jtag
        # WiFi stack: nvs_flash for credential storage scaffold,
        # esp_netif + esp_event for the netif lifecycle, esp_wifi for
        # the radio. Scan-only mode still pulls the whole stack.
        nvs_flash
        esp_netif
        esp_event
        esp_wifi
        # BLE — NimBLE host on top of the BT controller. Observer-only,
        # but we still link the full host library.
        bt
        # SD card: fatfs gives us esp_vfs_fat_sdcard_format() etc.
        # The BSP already pulls in sdmmc + esp_driver_sdmmc, but the
        # FATFS layer is its own component.
        fatfs
        # System scene + settings module.
        app_update
        esp_hw_support
        driver
        spi_flash
        # Real-OTA path: esp_https_ota wraps esp_https_ota_perform() over
        # esp_http_client; both are needed at link time. mbedtls is pulled
        # in transitively for TLS validation of the firmware host.
        esp_https_ota
        esp_http_client
        mbedtls
        # ESP32-S3 SoC has an internal temperature sensor; the driver
        # split out into its own component in IDF v5+.
        esp_driver_tsens
)

# Hint LVGL to use the simple include form `#include "lvgl.h"`.
idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
if(LVGL_LIB)
    target_compile_options(${LVGL_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE)
endif()
