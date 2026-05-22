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
        # Waveshare AMOLED 2.16 — pulled from ${ESP_HARNESS_ROOT}/boards/
        # via the top-level CMakeLists' EXTRA_COMPONENT_DIRS.
        esp32_s3_touch_amoled_2_16
    PRIV_REQUIRES
        nvs_flash
        esp_timer
)
