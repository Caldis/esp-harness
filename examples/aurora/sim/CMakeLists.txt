# examples/aurora/sim/CMakeLists.txt — host LVGL build of selected Aurora scenes.
#
# Layout assumption (monorepo v1.5+):
#   esp-harness/
#     components/aurora-harness/      ← scene framework + console protocol
#     sim-base/                        ← board-agnostic stubs + mock_bsp + SDL2 base
#     examples/aurora/
#       main/                          ESP-IDF target source
#       managed_components/lvgl__lvgl/ LVGL (populated by `idf.py reconfigure`)
#       sim/                           ← (you are here)
#
# Override `ESP_HARNESS_ROOT` if Aurora is consumed from outside the monorepo
# (e.g. as a submodule). Override `LVGL_DIR` if LVGL lives somewhere unusual.

cmake_minimum_required(VERSION 3.16)
project(aurora_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT DEFINED ESP_HARNESS_ROOT)
    set(ESP_HARNESS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
endif()
set(SIM_BASE          ${ESP_HARNESS_ROOT}/sim-base)
set(HARNESS_COMPONENT ${ESP_HARNESS_ROOT}/components/aurora-harness)

# SDL2: prefer raw -DSDL2_PREFIX over find_package, because the upstream
# `sdl2-devel-*-mingw.zip` ships a sdl2-config.cmake with hardcoded build
# paths (`/tmp/tardir/...`) that fail set_and_check on any other machine.
# Pass: -DSDL2_PREFIX=C:/path/to/x86_64-w64-mingw32
if(DEFINED SDL2_PREFIX)
    set(SDL2_INCLUDE_DIRS "${SDL2_PREFIX}/include")
    # MinGW + SDL2 link order is load-bearing: -lmingw32 must come BEFORE
    # -lSDL2main (which provides main->WinMain shim), and SDL2main before
    # SDL2. Reordering breaks with `undefined reference to WinMain`.
    set(SDL2_LIBRARIES    mingw32 "${SDL2_PREFIX}/lib/libSDL2main.a" "${SDL2_PREFIX}/lib/libSDL2.dll.a")
    set(SDL2_BINDIR       "${SDL2_PREFIX}/bin")
    message(STATUS "Using SDL2 prefix: ${SDL2_PREFIX}")
else()
    find_package(SDL2 REQUIRED)
endif()

# Locate LVGL. Prefer the Aurora example's managed_components (target-side
# fetch), allow override via -DLVGL_DIR=...
if(NOT DEFINED LVGL_DIR)
    set(LVGL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/lvgl__lvgl")
endif()

if(NOT EXISTS "${LVGL_DIR}/lvgl.h")
    message(FATAL_ERROR
        "LVGL not found at ${LVGL_DIR}. Either run `idf.py reconfigure` from "
        "examples/aurora/ first (populates managed_components/lvgl__lvgl), or "
        "clone LVGL manually and pass -DLVGL_DIR=/path/to/lvgl. See "
        "examples/aurora/sim/README.md.")
endif()

message(STATUS "esp-harness root: ${ESP_HARNESS_ROOT}")
message(STATUS "Using LVGL from: ${LVGL_DIR}")

add_definitions(-DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE)
include_directories(
    ${SIM_BASE}/include                    # ESP-IDF stubs (esp_err / esp_log / freertos / bsp)
    ${CMAKE_CURRENT_SOURCE_DIR}            # lv_conf.h, mock_peripherals.h
    ${SIM_BASE}                            # mock_bsp.h
    ${CMAKE_CURRENT_SOURCE_DIR}/..         # examples/aurora/main/ visible as ../main
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/peripherals
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/scenes
    ${HARNESS_COMPONENT}/include           # harness/scene_framework.h etc.
    ${LVGL_DIR}
    ${LVGL_DIR}/src
    ${SDL2_INCLUDE_DIRS}                   # /include — for <SDL2/SDL.h>
)
# Linux apt-installed SDL2 sets SDL2_INCLUDE_DIRS = /usr/include/SDL2
# directly, so <SDL.h> resolves with no extra path. MinGW dev zip uses
# /include + a SDL2/ subdir — add that only when it actually exists.
foreach(d ${SDL2_INCLUDE_DIRS})
    if(EXISTS "${d}/SDL2")
        include_directories("${d}/SDL2")   # for bare <SDL.h> on Windows
    endif()
endforeach()

# LVGL: glob the source tree. LVGL's CMake support varies by version, so
# globbing is the simplest portable approach for this scaffold.
file(GLOB_RECURSE LVGL_SRC
    "${LVGL_DIR}/src/*.c"
)

# Scenes — host-portable subset of main/scenes/. Excluded for now:
#   Echo / Vault / Whisper / Spectrum / Listen / Survey / Sniff
# (deep peripheral coupling: async audio task, SD+fatfs, BLE/WiFi
#  init/deinit dance, microphone DMA, scan-task lifecycle).
set(AURORA_SCENES
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/scenes/scene_halo.c"     # I
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/scenes/scene_grid.c"     # II
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/scenes/scene_bloom.c"    # III
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/scenes/scene_tilt.c"     # IV  — mouse drives accel
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/scenes/scene_pulse.c"    # V   — fake charging state
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/scenes/scene_cell.c"     # X   — pmic dashboard
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/scenes/scene_keys.c"     # XI  — SDL key 1/2 → BOOT/USER
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/scenes/scene_tone.c"     # XIII— audio tone (no-op sound)
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/scenes/scene_system.c"   # XIV — static SoC info
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/scenes/scene_glow.c"     # XV  — brightness logged
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/scenes/scene_spin.c"     # XVI — mouse drives gyro
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/scenes/scene_notify.c"   # XIX — toast demo
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/scenes/scene_track.c"    # XX  — progress demo
)

# Aurora framework files that don't touch ESP-IDF directly. v1.4:
# scene_framework lives in components/aurora-harness — we compile its
# source directly here (no IDF component manager on host) to keep the
# sim build a single executable.
set(AURORA_FRAMEWORK
    "${HARNESS_COMPONENT}/src/scene_framework.c"
    "${HARNESS_COMPONENT}/src/toast.c"
    "${HARNESS_COMPONENT}/src/progress.c"
    "${SIM_BASE}/mock_bsp.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/ui_shell.c"
)

add_executable(aurora_sim
    main.c
    mock_peripherals.c
    ${AURORA_SCENES}
    ${AURORA_FRAMEWORK}
    ${LVGL_SRC}
)

target_link_libraries(aurora_sim PRIVATE ${SDL2_LIBRARIES})

if(WIN32 AND DEFINED SDL2_BINDIR)
    add_custom_command(TARGET aurora_sim POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SDL2_BINDIR}/SDL2.dll"
            "$<TARGET_FILE_DIR:aurora_sim>"
        VERBATIM
    )
endif()
