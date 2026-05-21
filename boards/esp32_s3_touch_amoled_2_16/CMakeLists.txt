idf_component_register(
    SRCS "esp32_s3_touch_amoled_2_16.c" ${SRC_VER}
    INCLUDE_DIRS "include" "include/bsp"
    PRIV_INCLUDE_DIRS "priv_include"
    # ESP-IDF v6.0 splits drivers into per-peripheral components. BSP's public
    # header includes driver/i2s_std.h, driver/sdmmc_host.h, driver/i2c_master.h —
    # so anyone consuming the BSP transitively needs all of them visible.
    REQUIRES driver
             esp_driver_gpio
             esp_driver_i2c
             esp_driver_i2s
             esp_driver_sdmmc
             esp_lcd
    # NOTE: 'usb' removed — was an unused PRIV_REQUIRES that broke under v6.0
    # (legacy 'usb' component moved to component registry; BSP doesn't use it).
    PRIV_REQUIRES esp_timer spiffs esp_psram fatfs
)
