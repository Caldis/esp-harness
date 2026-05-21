# `sim/` — host LVGL build, adoption guide

> How to take this directory and use it as the host simulator for **your
> own** ESP-IDF + LVGL project. Not "what does Aurora do" — that's in
> `sim/README.md`. This is the scaffold-mindset version.

The `sim/` tree is intentionally generic. Only `mock_peripherals.{c,h}`
and the included scenes are Aurora-specific. Everything else — the
CMake setup, the ESP-IDF stub headers, the LVGL+SDL2 entry point, the
host BSP mock, the snapshot/diff plumbing — works for any ESP32 +
LVGL project that uses the [`aurora-harness`](../components/aurora-harness/README.md)
scene framework.

---

## What's generic, what's Aurora-specific

```
sim/
├── CMakeLists.txt              GENERIC — only AURORA_SCENES + project name need editing
├── lv_conf.h                   GENERIC — mirror your target's lv_conf
├── main.c                      GENERIC — scene_fw + SDL2 + key-event glue
├── host_esp_shim.h             GENERIC — pre-include shim (unused, kept for ref)
├── mock_bsp.{c,h}              GENERIC — bsp_display_{lock,unlock,brightness}
├── mock_peripherals.{c,h}      AURORA-SPECIFIC — imu/pmic/audio/wifi/ble/sd/keys mocks
├── include/                    GENERIC — ESP-IDF stub headers
│   ├── esp_err.h               (esp_err_t, ESP_OK, esp_err_to_name)
│   ├── esp_log.h               (ESP_LOG{I,W,E,D} -> printf)
│   ├── esp_timer.h             (esp_timer_get_time backed by SDL_GetTicks)
│   ├── esp_heap_caps.h         (heap_caps_* -> stdlib malloc)
│   ├── esp_system.h            (esp_restart, esp_reset_reason)
│   ├── bsp/esp-bsp.h           (re-exports mock_bsp.h)
│   ├── bsp/display.h           (same)
│   └── freertos/...            (FreeRTOS.h, task.h, semphr.h stubs)
├── golden/                     PROJECT-SPECIFIC — your visual regression baseline
└── tools/setup.ps1             GENERIC — prereq detector
```

Aurora-specific bits live in `mock_peripherals.*` and `golden/`. Keep
everything else.

---

## Adoption checklist for a new project

Assumes your project follows the Aurora pattern: ESP-IDF + LVGL,
scenes defined in `main/scenes/scene_*.c`, peripherals in
`main/peripherals/*.{h,c}`, depends on the `aurora-harness` component.

1. **Copy the entire `sim/` directory** into your project root.
   ```bash
   cp -r path/to/esp32-harness-showcase/sim my-project/
   ```

2. **Update `sim/CMakeLists.txt`**:
   - Change `project(aurora_sim C)` to `project(my_sim C)`.
   - Edit the `AURORA_SCENES` list to point at your project's
     `main/scenes/scene_*.c` files. Start small (one pure-render
     scene) and add more as you mock their peripheral deps.
   - The `AURORA_FRAMEWORK` list keeps `aurora-harness`'s
     `scene_framework.c` as a direct source compile (we don't run
     ESP-IDF's component manager on the host).

3. **Replace `mock_peripherals.{c,h}`** with your project's
   peripheral mocks. The pattern: for each
   `main/peripherals/foo.{h,c}` you have, write a host-side
   `foo_*` stub that matches the public signatures byte-for-byte
   (the compiler will reject any drift the moment you include the
   real header).

   Tip: include the **real** header in your stub file, then implement
   each exported function. That way signature changes break
   compilation immediately, not at runtime.

   ```c
   /* mock_peripherals.c */
   #include "peripherals/foo.h"   /* real header — guarantees signature match */

   bool foo_init(void) { return true; }
   void foo_read(int *out) { if (out) *out = 0; }
   ```

4. **Edit `sim/main.c`'s scene registration block** to register the
   scenes you compiled in (the names and count must match
   `AURORA_SCENES`).

5. **Edit `sim/lv_conf.h`** to match your target's font sizes and
   widget set. Specifically: every `lv_font_montserrat_NN` your scenes
   reference needs `LV_FONT_MONTSERRAT_NN 1` in `lv_conf.h`.

6. **Build it**. See `sim/README.md` for the exact PowerShell / Linux
   commands. On a fresh setup the first build is the slowest part —
   gcc compiles every LVGL `.c` file (no precompiled lib).

7. **Capture initial golden snapshots**:
   ```bash
   esp-harness sim update-golden --scenes a,b,c \
       --golden sim/golden \
       --bin sim/build/aurora_sim.exe
   ```
   Commit the BMPs.

---

## Mock pattern: making more scenes runnable

Pure-LVGL scenes (no peripheral reads, no FreeRTOS task spawn) work
out of the box. Anything that reads a sensor / button / radio needs
a mock.

### Static value mocks

Trivial — just return a fixed plausible value:

```c
/* peripherals/pmic.h declares: void pmic_get(pmic_state_t *out); */
void pmic_get(pmic_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->ready = true;
    out->percent = 87;
    out->voltage_mv = 4135;
    /* etc */
}
```

Use this when the scene just displays the value. Battery dashboards,
SoC info, version strings — all good fits.

### SDL-driven mocks

If the scene visibly *responds* to peripheral input, route SDL events
to your mock's internal state:

```c
/* IMU accelerometer fed by SDL mouse position — Tilt scene becomes
 * draggable. Read in mock_peripherals.c::imu_get_accel. */
static void mouse_to_tilt(float *ax, float *ay)
{
    int mx, my, ww = 466, wh = 466;
    SDL_Window *win = SDL_GetMouseFocus();
    if (win) SDL_GetWindowSize(win, &ww, &wh);
    SDL_GetMouseState(&mx, &my);
    /* normalise to [-1, 1] then scale to ~0.7 g max */
    float nx = ((float)mx / ww - 0.5f) * 2.0f;
    float ny = ((float)my / wh - 0.5f) * 2.0f;
    if (ax) *ax = nx * 0.7f;
    if (ay) *ay = ny * 0.7f;
}
```

Keyboard for digital input — wire SDL keydown/keyup events in
`main.c`'s event loop to the mock:

```c
/* sim/main.c */
case SDLK_1: mock_keys_set_boot(true);  break;   /* KEYDOWN */
case SDLK_1: mock_keys_set_boot(false); break;   /* KEYUP */
```

Then your `keys_get(&state)` mock returns whatever the SDL event loop
set.

### Async-task mocks

Scenes that spawn `xTaskCreate(...)` for a long-running peripheral
operation (BLE scan, audio playback, SD bench) are the **hardest** to
mock. Two approaches:

- **Easy / partial**: `freertos/task.h` stub already implements
  `xTaskCreate(fn, ...)` as a synchronous inline call to `fn(arg)`.
  This works if the function eventually returns (via `vTaskDelete`).
  For Aurora's BLE/WiFi scenes, this *almost* works but `lv_async_call`
  inside the "task" then dispatches to LVGL which doesn't really tick
  the same way on host.
- **Hard / full**: write a `pthread` based mock task scheduler. Out of
  scope for the v1.2 scaffold — for now, just don't compile such
  scenes into the sim build.

The Aurora `sim/CMakeLists.txt` skip-list (`Echo / Vault / Whisper /
Spectrum / Listen / Survey / Sniff`) is exactly the "won't compile
without an async-task mock" group.

---

## ESP-IDF stub headers: the philosophy

`sim/include/` is **not** a full reimplementation of ESP-IDF. It's the
**smallest set of declarations** that lets firmware source compile
on the host. Concretely:

- `esp_err.h` declares `esp_err_t` (int) and constants. No real error
  propagation; everything that returns `esp_err_t` returns `ESP_OK`.
- `esp_log.h` macros expand to `printf` / `fprintf(stderr,...)`. The
  format strings keep working unchanged.
- `freertos/task.h` provides `xTaskCreate` (inline-call), `vTaskDelay`
  (mapped to `SDL_Delay`), `vTaskDelete` (no-op).
- `bsp/esp-bsp.h` re-exports `mock_bsp.h`, which provides
  no-op `bsp_display_lock` / `bsp_display_unlock` and a printf-logged
  `bsp_display_brightness_set`.

**When to add a new stub**: only when you hit "fatal error: foo.h:
No such file or directory". Don't pre-populate. Each stub is a
maintenance cost — keep the set minimal.

---

## Why BMP and not PNG

`SDL_SaveBMP` is a single-line, zero-dep call. BMP is bigger on disk
(~868 KB per 466×466×32bit shot) but storage isn't a concern in CI.
The benefit: no external image library on the device side, no PIL
dep in the sim binary. Conversion to PNG (e.g. for sharing screenshots
with humans) happens in the host harness:

```powershell
Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Image]::FromFile("scene.bmp")
$img.Save("scene.png", [System.Drawing.Imaging.ImageFormat]::Png)
```

Or via PIL in a Python script. The toolkit's `sim diff` reads BMPs
directly via Pillow (which Aurora's toolkit already depends on for
`screenshot.py`).

---

## Gotchas (sealed from the first end-to-end build)

These are real bugs hit during sim/ bring-up. Recorded here so the
next host build doesn't trip on them.

| Symptom | Cause | Fix |
|---|---|---|
| `undefined reference to WinMain` on link | MinGW + SDL2 link order: must be `-lmingw32 → -lSDL2main → -lSDL2` | Hardcode order in `SDL2_LIBRARIES` (see `CMakeLists.txt`) |
| `set_and_check fails on /tmp/tardir/...` from sdl2-config.cmake | Upstream `SDL2-devel-mingw.zip` ships non-relocatable cmake config | Skip `find_package`, accept `-DSDL2_PREFIX` directly |
| `conflicting types for foo_*` | Mock signature drifted from real header | Include the real header in your mock — compiler enforces |
| `lv_font_montserrat_NN undeclared` | New scene uses a font size not enabled in `lv_conf.h` | Add `#define LV_FONT_MONTSERRAT_NN 1` |
| Scene 0 always shows even after `--scene N` | LVGL hasn't ticked between scene_fw_register and the requested switch | Call `lv_timer_handler()` once more before `SDL_SaveBMP` (already done in `main.c`) |
| `scene <id>` works in firmware but not in sim | Sim's id support depends on `scene_fw_get` — make sure `scenes.h` exports all scene objects | Check scene linkage; missing externs silent-fail |
| `bsp_display_lock(0)` hangs console task ~5s + WDT | Non-blocking try-lock fails silently on busy system; subsequent LVGL calls spin | Always use `bsp_display_lock(-1)` from non-LVGL tasks |

---

## Limitations of the sim

Honest list of what the sim cannot tell you about the real device:

- **Performance**. Host gcc is way faster than Xtensa, your FPS
  measurements here mean nothing.
- **Memory layout**. PSRAM vs DRAM behaviour, DMA alignment,
  internal-SRAM exhaustion — none reproduce on host.
- **Real peripheral timing**. Audio buffer underruns, I²C clock
  stretches, SD card slow paths — sim short-circuits all of these.
- **Power state transitions**. Sleep/wake, brownout, OTA partition
  switch — sim doesn't simulate.
- **WiFi/BLE coexistence**. Sim has empty scan results; the real
  internal-SRAM-pool fight between the two radios is hardware-level.
- **Display driver quirks**. The CO5300 SPI-DMA TX underflow that
  forced our 30 Hz cap — sim renders at whatever rate SDL gives
  (typically vsync, ~60 Hz). UI fluidity here is not a guarantee
  on-device.

**The sim catches**: layout regressions, font sizing, color choices,
text overflow, widget logic bugs, scene-framework lifecycle issues.
That's already most of UI iteration time.
