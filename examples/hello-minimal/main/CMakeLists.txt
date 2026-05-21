idf_component_register(
    SRCS
        "hello_minimal_main.c"
        "scenes/scene_hello.c"
    INCLUDE_DIRS
        "."
        "scenes"
    REQUIRES
        lvgl__lvgl
        aurora-harness
        # Add your board's BSP component here. Aurora demo uses:
        # esp32_s3_touch_amoled_2_16
    PRIV_REQUIRES
        nvs_flash
        esp_timer
)
