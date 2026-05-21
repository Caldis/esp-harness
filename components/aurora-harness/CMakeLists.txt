# aurora-harness — line-protocol console + LVGL scene framework.
#
# Two cohesive modules that anchor the AI-driven dev loop:
#
#   - console_protocol  Line-based bidirectional serial I/O over UART /
#                       USB-Serial-JTAG with OK / ERR / EVT framing and
#                       multi-line payload tags. Register commands by
#                       handing a static console_cmd_t to
#                       console_protocol_register().
#
#   - scene_framework   Stage manager for a deck of LVGL "scenes" — each
#                       scene owns a full-screen container with optional
#                       per-frame tick, IMU push, long-press hook, etc.
#                       Scenes are registered once at boot; cycling
#                       through them is a brief crossfade.
#
# Both modules are LVGL-aware (scene_framework also calls bsp_display_lock
# during transitions) so we PRIV_REQUIRE the BSP + LVGL.
#
# Public headers live at include/harness/* so consumers do:
#   #include "harness/console_protocol.h"
#   #include "harness/scene_framework.h"
#
# See README.md for the integration walkthrough.

idf_component_register(
    SRCS
        "src/console_protocol.c"
        "src/scene_framework.c"
        "src/default_cmds.c"
        "src/toast.c"
        "src/screenshot.c"
        "src/progress.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # Public headers expose lv_obj_t * in scene_t and lv_color_t in
        # scene_t::accent — must be a public REQUIRES (not PRIV_REQUIRES)
        # so consumers can include the headers without separately
        # pulling LVGL.
        lvgl__lvgl
    PRIV_REQUIRES
        # v1.4: the component is now board-agnostic. We no longer hard-
        # require esp32_s3_touch_amoled_2_16 — the only BSP surface we
        # touch is bsp_display_lock / bsp_display_unlock (see
        # include/harness/bsp_iface.h), and the linker resolves those
        # via whatever BSP component the consuming app uses. See
        # PORTING.md for the multi-board details.
        esp_driver_usb_serial_jtag
        # default_cmds.c: ?stat uses heap_caps_* + esp_timer_get_time;
        # screenshot.c uses lv_async_call + FreeRTOS semaphore.
        esp_timer
)
