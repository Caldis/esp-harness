# Porting aurora-harness to a new board

`aurora-harness` is intentionally board-agnostic. The whole hard
dependency on a specific BSP is **two function symbols**:

```c
bool bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);
```

If your project consumes one of the standard Espressif BSPs (Waveshare,
M5Stack, LilyGO, etc.), these are already there — no work needed,
just include the BSP component in your `main`'s `REQUIRES`. The link
phase resolves the symbols automatically.

The remainder of this document is for projects that:
- Use a custom hand-rolled display driver (no BSP component), **or**
- Want to bring up aurora-harness on a board not yet covered by a
  vendor BSP, **or**
- Are running on the host simulator (`sim/`) which uses
  `sim/mock_bsp.{h,c}` as a no-op implementation.

---

## Minimum BSP shim (one source file)

Drop something like this into your project's `main/` (or a new
`components/myboard-bsp/`):

```c
/* myboard_bsp.c */
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lvgl_mutex;

void myboard_bsp_init_lock(void)
{
    /* Call once before bringing up LVGL. */
    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == (uint32_t)-1) ? portMAX_DELAY
                       : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mutex, ticks) == pdTRUE;
}

void bsp_display_unlock(void)
{
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}
```

Then call `myboard_bsp_init_lock()` early in `app_main` (before
`scene_fw_init` or any LVGL operation that might be touched from a
non-LVGL task).

The mutex **must be recursive** — the LVGL task itself takes it during
its own callbacks (via the BSP's standard wrapper) and our console
handlers take it again on top.

---

## What you still need beyond the lock

The LVGL setup itself is your board's responsibility (display driver,
buffer allocation, indev / touch). Once `lv_screen_active()` returns a
valid object, aurora-harness's `scene_fw_init(lv_screen_active())`
takes over from there.

Concretely a typical bring-up sequence looks like:

```c
void app_main(void)
{
    nvs_flash_init();

    /* 1. Your board's display + LVGL init. */
    myboard_bsp_init_lock();
    my_display_init();
    lv_init();
    my_register_lvgl_display();
    my_register_lvgl_touch();
    lv_tick_set_cb(...);

    /* 2. aurora-harness — board-agnostic from here on. */
    console_protocol_init();
    harness_default_register();

    bsp_display_lock((uint32_t)-1);
    scene_fw_init(lv_screen_active());
    scene_fw_set_change_listener(my_chrome_sync_cb);
    scene_fw_register(&my_scene_main);
    /* more scenes... */
    bsp_display_unlock();

    /* 3. App-specific peripherals + commands. */
}
```

`bsp_display_lock(-1)` is the recommended timeout for non-LVGL-task
callers (console handlers, sensor tasks, etc). The framework itself
takes care to call you back on the LVGL task where the lock is
already held — see `scene_change_listener_t` in `scene_framework.h`.

---

## What aurora-harness *doesn't* assume

- **A specific display resolution.** The framework asks
  `lv_obj_get_width(scr)` at runtime; everything else flows from there.
  The host simulator runs at 466×466, the Aurora target runs at 466×466
  too, but porting to 240×240 or 800×480 needs no change here.
- **A specific colour format.** `?dump` snapshots whatever LVGL hands
  out, currently RGB565. If your LVGL is built for ARGB8888 you'll need
  to tweak `screenshot.c` (it's annotated where the format hardcoding
  happens).
- **A touch panel.** Touch is optional — without it, the framework
  still runs, you just can't tap-to-cycle scenes. Console-driven scene
  switching (`scene next` etc.) still works.
- **WiFi / BLE / SD / audio.** All app-side. Mock them in the host
  simulator via `sim/mock_peripherals.{h,c}` if you want host parity.

---

## Host simulator parity

`sim/include/bsp/esp-bsp.h` re-exports `sim/mock_bsp.h`, which provides
no-op `bsp_display_lock(0) {}` and `bsp_display_unlock(void) {}`. SDL2
handles thread safety on the host, so no real mutex is needed.

If your real-board BSP adds extra functions (`bsp_display_brightness_set`,
etc.) and you reference them from scenes, mirror them in
`sim/mock_bsp.c` as no-op stubs. See `sim/INTEGRATION.md` §"Mock
pattern" for the recipe.

---

## Checklist for a fresh board bring-up

- [ ] LVGL up; `lv_screen_active()` returns non-NULL after init
- [ ] A recursive mutex protects LVGL widget operations across tasks
- [ ] `bsp_display_lock(uint32_t timeout_ms) -> bool` matches header
- [ ] `bsp_display_unlock(void)` releases it
- [ ] Console output goes somewhere a host can read (UART / USB-Serial-JTAG)
- [ ] `console_protocol_init()` succeeds (check serial for `?ping` -> `OK: pong`)
- [ ] `scene_fw_init` + one registered scene renders + tap cycles to a
      second scene
- [ ] `?stat` returns JSON; `scene list` returns the manifest
- [ ] `?dump w=128` returns a base64 payload

When all eight are green you have aurora-harness fully ported.
