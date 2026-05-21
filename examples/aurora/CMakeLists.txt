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
