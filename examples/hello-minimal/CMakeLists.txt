# Top-level ESP-IDF project file for hello_minimal.
#
# This is the smallest possible esp-harness consumer — a project with
# one scene, one console command, and no peripheral drivers. Lives at
# `examples/hello-minimal/` in the monorepo.
#
# Path resolution: since this example lives inside the monorepo, we
# locate aurora-harness via a relative path. For projects OUTSIDE the
# monorepo, override with `-DESP_HARNESS_ROOT=/abs/path/to/esp-harness`.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ESP_HARNESS_ROOT)
    set(ESP_HARNESS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
endif()
list(APPEND EXTRA_COMPONENT_DIRS
    ${ESP_HARNESS_ROOT}/components
    ${ESP_HARNESS_ROOT}/boards
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hello_minimal)
