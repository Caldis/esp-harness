# Aurora simulator (host LVGL build)

A native Windows / Linux / macOS build of selected Aurora scenes, running
LVGL on top of SDL2. Designed for fast UI iteration without flashing.

## Why this exists

Edit/build/flash/test cycle on the ESP32-S3 is ~60 s. Edit/build/run on
the host is ~3 s. For pure-UI changes (typography, layout, colour, font
loading, animation timing) the host build is 20× faster.

The host build is intentionally a **subset**: only scenes that don't
touch real peripherals can run here (Halo, Grid, Bloom today; tilt-able
scenes once we mock IMU). Peripheral-coupled scenes (audio loopback, SD
bench, BLE scan) must stay on-target.

## Status: working

End-to-end build verified on Windows 11 with **scoop**-installed MinGW
gcc 16.1.0 + SDL2 2.30.10. `aurora_sim.exe` opens a 466×466 window.

13 of the 20 scenes are host-renderable today (halo / grid / bloom /
tilt / pulse / cell / keys / tone / system / glow / spin / notify /
track) — these are the ones the CI sim-diff workflow gates on.
The other 7 (echo / vault / whisper / spectrum / listen / survey /
sniff) depend on hardware peripherals (audio / SD / BLE / WiFi /
mic) and are stubbed-out at compile time via `mock_peripherals.{c,h}`.
The mock pattern is what you copy when adding host parity for a new
peripheral.

## Setup (verified path — scoop, user-mode)

`scoop` installs into `%USERPROFILE%\scoop\` without admin. The other
common choices (chocolatey, vcpkg, manual SDL2-devel zip) all also work;
this is just the path that's been tested end-to-end.

```powershell
# 1. Install scoop if not present (user profile only, no admin needed)
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser -Force
Invoke-RestMethod -Uri https://get.scoop.sh | Invoke-Expression

# 2. MinGW compiler (gcc 16.1+)
scoop install mingw

# 3. SDL2 dev libs — the upstream sdl2-config.cmake has hardcoded build
# paths, so we download the dev tarball + pass -DSDL2_PREFIX to our
# CMakeLists. Drop SDL2-2.30.x dev zip anywhere; the example below uses
# scoop/apps/sdl2/ to match `tools/setup.ps1`.
$dest = "$env:USERPROFILE\scoop\apps\sdl2"
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Invoke-WebRequest `
    -Uri "https://github.com/libsdl-org/SDL/releases/download/release-2.30.10/SDL2-devel-2.30.10-mingw.zip" `
    -OutFile "$env:TEMP\sdl2.zip"
Expand-Archive -Path "$env:TEMP\sdl2.zip" -DestinationPath $dest -Force
```

CMake comes from the ESP-IDF EIM install (`C:\Espressif\tools\cmake\<ver>\bin\`).

## Build

```powershell
$env:PATH = "$env:USERPROFILE\scoop\shims;$env:USERPROFILE\scoop\apps\mingw\current\bin;C:\Espressif\tools\cmake\4.0.3\bin;$env:PATH"
$sdl2 = "$env:USERPROFILE\scoop\apps\sdl2\SDL2-2.30.10\x86_64-w64-mingw32"

cd sim
cmake -B build -G "MinGW Makefiles" -DSDL2_PREFIX=$sdl2
cmake --build build -j
.\build\aurora_sim.exe                  # opens a 466×466 window
```

The SDL2.dll is auto-copied next to the exe by a POST_BUILD step.

## Gotchas (from the first end-to-end build, sealed here)

* **SDL2 link order is load-bearing.** MinGW needs `-lmingw32` BEFORE
  `-lSDL2main` BEFORE `-lSDL2`. Reorder = `undefined reference to WinMain`.
* **`sdl2-config.cmake` from upstream is non-relocatable.** It hardcodes
  `/tmp/tardir/...` build paths. We bypass `find_package` and consume
  `-DSDL2_PREFIX=...` directly. See `CMakeLists.txt`.
* **ESP-IDF headers stubbed in `sim/include/`.** scene_framework /
  ui_shell / peripheral .c files include `esp_log.h`, `bsp/esp-bsp.h`,
  `freertos/FreeRTOS.h`, etc. The stubs are tiny — printf for logs,
  no-ops for tasking. First on the include path so the firmware sources
  resolve transparently.
* **Match peripheral mock signatures to the real headers byte-for-byte.**
  `void imu_get_accel(float*, float*, float*)` not `bool …` — even if
  the void version is more honest. The compiler will reject signature
  drift the moment you reuse a real header.

## What's mocked

| ESP-IDF API | Host stub | Notes |
|---|---|---|
| `bsp_display_lock` / `unlock` | no-op | LVGL's own thread-safety covers what we need on host |
| `bsp_display_brightness_set` | no-op + printf | brightness has no meaning on a windowed app |
| `imu_*` | returns zero readings | scenes that read IMU show neutral pose |
| `pmic_*` | returns "100%, 4150 mV, idle" | dashboard scenes show static placeholder |
| `audio_*` | no-op | scene_listen etc. won't compile against this yet |
| `wifi_*`, `ble_*` | empty scan results | Spectrum/Whisper show empty state |
| `sdcard_*` | "no card" | Vault shows ejected state |
| `keys_*` | reads via SDL keyboard (1/2 -> BOOT/USER, ESC -> quit) | |
| `settings_*` | in-memory cache only | no NVS persistence |

## How a host run looks (intended)

1. `aurora_sim.exe` opens a 466×466 borderless window.
2. The same scene framework runs, defaulting to scene 0 (Halo).
3. **Tap / click** anywhere → next scene.
4. **1** key → simulates BOOT (rescan / cycle / etc., per scene).
5. **2** key → simulates USER.
6. **Esc** → close.

## How the toolkit will drive this (Phase D3, not yet wired)

```bash
esp-harness sim run --scenes halo,grid,bloom --snapshot-dir ./snaps
```

Each named scene is shown, `lv_snapshot_take` dumps the framebuffer as
PNG, the process exits. The toolkit then diffs against
`sim/golden/<scene>.png` for visual regression detection in CI.

## Files

| File | Purpose |
|---|---|
| `CMakeLists.txt` | host build config |
| `lv_conf.h` | LVGL config for host (SDL display driver enabled) |
| `main.c` | entry: SDL init, LVGL init, register scenes, event loop |
| `mock_bsp.h` / `mock_bsp.c` | stubs for `bsp_display_*` and `bsp/esp-bsp.h` |
| `mock_peripherals.h` / `mock_peripherals.c` | stubs for imu/pmic/audio/wifi/ble/sdcard/keys/settings |
| `tools/setup.ps1` | optional helper to detect / install SDL2 |
