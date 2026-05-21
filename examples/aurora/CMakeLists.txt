cmake_minimum_required(VERSION 3.5)

# Aurora — generative-art companion for the Waveshare ESP32-S3-Touch-AMOLED-2.16.
# This is one of the examples in the esp-harness monorepo. It consumes the
# reusable `aurora-harness` component and the vendored Waveshare BSP from the
# parent layout:
#
#   <repo-root>/
#     components/aurora-harness/             ← scene framework + console protocol
#     boards/esp32_s3_touch_amoled_2_16/     ← vendored Waveshare BSP
#     examples/aurora/                       ← (you are here)
#
# Override either via -DESP_HARNESS_ROOT=... if Aurora is consumed from outside
# the monorepo (e.g. as a git submodule).

if(NOT DEFINED ESP_HARNESS_ROOT)
    set(ESP_HARNESS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
endif()

list(APPEND EXTRA_COMPONENT_DIRS
    ${ESP_HARNESS_ROOT}/components
    ${ESP_HARNESS_ROOT}/boards
)

# ── Known-upstream patches for managed_components ─────────────────────
#
# v1.7+ : Auto-patch fetched managed_components/ on every configure to
# work around two known issues with waveshare/qmi8658 v1.0.0:
#
#   1. CMakeLists.txt only REQUIRES "driver" (v5-era umbrella); ESP-IDF
#      v6 split out i2c_master into esp_driver_i2c. Without the explicit
#      requirement, qmi8658.c can't find driver/i2c_master.h.
#   2. qmi8658.h defines M_PI without an ifndef guard, which gcc 15+
#      treats as a builtin-macro-redefined error.
#
# Both are upstream bugs we're patching here. The function is idempotent
# (signature-checks before re-writing) so it's safe across reconfigures.
# Caveat: the FIRST configure on a fresh clone populates managed_components/
# AFTER cmake parses CMakeLists, so the very first build still fails. The
# `esp-harness build` wrapper detects the failure pattern and retries —
# users invoking `idf.py` directly need to `idf.py reconfigure` twice.
function(esp_harness_apply_known_patches)
    set(qmi_cmake "${CMAKE_CURRENT_SOURCE_DIR}/managed_components/waveshare__qmi8658/CMakeLists.txt")
    if(EXISTS "${qmi_cmake}")
        file(READ "${qmi_cmake}" _content)
        if(NOT _content MATCHES "esp_driver_i2c")
            message(STATUS "esp-harness: patching waveshare__qmi8658 CMakeLists.txt (adds esp_driver_i2c)")
            file(WRITE "${qmi_cmake}"
"# Auto-patched by examples/aurora/CMakeLists.txt — see esp_harness_apply_known_patches.
idf_component_register(
    SRCS \"qmi8658.c\"
    INCLUDE_DIRS \"include\"
    REQUIRES \"driver\" \"esp_driver_i2c\"
)
")
        endif()
    endif()

    set(qmi_header "${CMAKE_CURRENT_SOURCE_DIR}/managed_components/waveshare__qmi8658/include/qmi8658.h")
    if(EXISTS "${qmi_header}")
        file(READ "${qmi_header}" _hcontent)
        if(NOT _hcontent MATCHES "#ifndef M_PI")
            message(STATUS "esp-harness: patching qmi8658.h (adds M_PI ifndef guard)")
            string(REPLACE
                "#define M_PI (3.14159265358979323846f)"
                "#ifndef M_PI\n#define M_PI (3.14159265358979323846f)\n#endif"
                _patched "${_hcontent}")
            file(WRITE "${qmi_header}" "${_patched}")
        endif()
    endif()
endfunction()
esp_harness_apply_known_patches()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aurora)

add_compile_options(
    -Wno-format
    # gcc 15.2+ flags M_PI redefinition in waveshare__qmi8658 header. The
    # header guards aren't perfect upstream; downgrading just this warning
    # is safer than -Wno-error=all.
    -Wno-error=builtin-macro-redefined
    -Wno-builtin-macro-redefined
)
