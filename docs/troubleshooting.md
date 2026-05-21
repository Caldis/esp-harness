# Troubleshooting

Symptom-based fixes for things that go wrong. If your symptom isn't
here, check [`faq.md`](./faq.md) for conceptual questions, then open
an [Issue](https://github.com/Caldis/esp-harness/issues/new?template=bug.md)
with `esp-harness doctor --json` output.

---

## Build / configure issues

### `cmake: command not found`

You don't have CMake on PATH. Options:

- **Use the one bundled with EIM**: it's at
  `C:\Espressif\tools\cmake\<ver>\bin\cmake.exe` on Windows. Add to
  PATH or use the absolute path.
- **Install separately**: `scoop install cmake` (Windows user-mode),
  `apt install cmake` (Ubuntu), `brew install cmake` (macOS).

### `Component "aurora-harness" defined in manifest file ... is not found`

ESP-IDF can't find the component. Causes:

1. **Wrong working directory**: are you running `idf.py build` from
   inside an example directory like `examples/aurora/`, not from the
   monorepo root?
2. **Wrong EXTRA_COMPONENT_DIRS path**: if you're using esp-harness
   from outside the monorepo, pass `-DESP_HARNESS_ROOT=/abs/path/to/esp-harness`
   to cmake.
3. **Wrong directory name**: ESP-IDF derives the component name from the
   directory name. `components/aurora-harness/` → component name
   `aurora-harness`. Don't rename the dir.

### `Compilation failed because qmi8658.h includes driver/i2c_master.h ... not in requirements list`

Upstream bug in `waveshare/qmi8658` v1.0.0. Patch:

```bash
# In your project root after first `idf.py reconfigure`:
cat > managed_components/waveshare__qmi8658/CMakeLists.txt << 'EOF'
idf_component_register(
    SRCS "qmi8658.c"
    INCLUDE_DIRS "include"
    REQUIRES "driver" "esp_driver_i2c"
)
EOF
```

We have an open task to auto-apply this in our top-level CMake.

### `qmi8658.h:18: error: 'M_PI' redefined`

Same upstream package, gcc 15+ enforcement. Patch the header:

```bash
sed -i '/^#define M_PI/c\#ifndef M_PI\n#define M_PI (3.14159265358979323846f)\n#endif' \
    managed_components/waveshare__qmi8658/include/qmi8658.h
```

### `idf.py: not recognized as an internal or external command`

ESP-IDF's environment isn't activated. Run the EIM activation script
once per shell session:

```powershell
# Windows / EIM
. C:\Espressif\tools\Microsoft.v<version>.PowerShell_profile.ps1
```

```bash
# Linux/macOS (or WSL with manual ESP-IDF install)
. $IDF_PATH/export.sh
```

`esp-harness build` handles this for you (it dot-sources the activation
script via subprocess). So just use that instead of `idf.py` directly.

### `Build for target 'esp32'` but you wanted ESP32-S3

Your `sdkconfig.defaults` doesn't pin a target. Either:

```bash
idf.py set-target esp32s3
```

or add to `sdkconfig.defaults`:

```ini
CONFIG_IDF_TARGET="esp32s3"
CONFIG_IDF_TARGET_ESP32S3=y
```

Aurora's defaults already have this in v1.5+.

---

## Flash / device issues

### `No ESP32 port found` from `esp-harness port detect`

- Verify the device is plugged in (`Device Manager` on Windows,
  `lsusb` on Linux, `ioreg -p IOUSB` on macOS).
- Try a different USB cable — a surprising number of USB-C cables are
  power-only.
- On Windows, install the [CP210x driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
  or [CH340 driver](http://www.wch.cn/downloads/CH341SER_EXE.html) if
  your board uses those USB-to-UART chips.

### `port busy: COMx`

Another process has the serial port open. Common culprits:

- An existing `esp-harness monitor` or `idf.py monitor` session
- Espressif's IDF Monitor in another VS Code window
- A serial terminal app (Putty / Tera Term / minicom)

Close all of those, then retry. If you can't find what's holding it,
unplug + replug the USB cable.

### Boot loops / panic / crashes

```bash
esp-harness backtrace --monitor 30 --json
```

This captures the next 30 s of output and runs `addr2line` against the
ELF automatically. Look for the `Backtrace:` lines + decoded source
locations.

### Console wedged — sends but no reply

```bash
esp-harness console --cmd "?ping" --timeout 10
```

If `?ping` doesn't return `OK: pong`, the firmware's console parser is
not responsive. Common causes:

1. **`bsp_display_lock(0)` deadlock** in a console handler — use `-1`,
   not `0` (see [`AGENT.md` §5](../AGENT.md#5-things-that-look-like-bugs-but-arent)).
2. **Long-running command handler** — anything ≥3 s blocks subsequent
   commands. Use `lv_async_call` to defer.
3. **Firmware crashed / rebooting** — check with `esp-harness monitor
   --seconds 5`.

---

## Sim build issues

### `undefined reference to WinMain` on MinGW Windows build

SDL2 link order is wrong. The order **must** be:

```cmake
target_link_libraries(your_target PRIVATE
    mingw32                                            # 1
    "${SDL2_PREFIX}/lib/libSDL2main.a"                 # 2
    "${SDL2_PREFIX}/lib/libSDL2.dll.a"                 # 3
)
```

`SDL2main` provides the `main` → `WinMain` shim and depends on
`mingw32`. Reordering breaks the link. Our `examples/aurora/sim/CMakeLists.txt`
gets this right; copy from there.

### `sdl2-config.cmake` error: `set_and_check fails on /tmp/tardir/...`

The upstream `SDL2-devel-mingw.zip` ships a non-relocatable
`sdl2-config.cmake` that references the original build machine's
paths. Don't use `find_package(SDL2)` against it.

Instead pass the prefix directly:

```bash
cmake -B build -DSDL2_PREFIX=$USERPROFILE/scoop/apps/sdl2/SDL2-2.30.10/x86_64-w64-mingw32
```

Our CMakeLists handles this via the `if(DEFINED SDL2_PREFIX)` branch.

### `lv_font_montserrat_NN undeclared`

Your `lv_conf.h` doesn't have that size enabled. In
`examples/<your-thing>/sim/lv_conf.h`:

```c
#define LV_FONT_MONTSERRAT_NN 1
```

Match whatever the target's lv_conf.h has — Aurora's includes 12, 14,
16, 18, 20, 22, 24, 26.

### `aurora_sim.exe` runs but window is blank

LVGL is up but no scene rendered. Check:

```bash
./build/aurora_sim --scene 0 --exit-after-ms 600 --snapshot test.bmp
```

If `test.bmp` exists and renders correctly: the GUI mode isn't getting
to the event loop. Probably hung in `scene_fw_init` or a scene's
`init` callback.

### sim diff shows >0% on all scenes after pulling latest

A chrome-layer change (e.g. another scene added → indicator dot
count changes) shifts every BMP. Run:

```bash
esp-harness sim update-golden --scenes halo,grid,bloom,...
```

Then commit the refreshed BMPs.

---

## Toolkit / Python issues

### `ModuleNotFoundError: No module named 'esp_harness'`

The CLI isn't installed. Re-run:

```bash
pip install -e tools/esp-harness/
```

If that fails: `python --version` should be ≥ 3.10. We don't support
3.9 or earlier.

### `esp-harness: command not found` after install

The pip install succeeded but the entry-point script isn't on PATH.
Possible causes:

- **You used `pip install --user`**: the script went to a user-bin
  directory that's not on PATH. Activate a venv first.
- **You're in the wrong shell**: `pip install -e` creates the script
  but doesn't reload `$PATH`. Open a new shell or run
  `python -m esp_harness <cmd>` directly.

### `esp-harness test` reports failed required checks

`esp-harness doctor` first — it'll tell you exactly what to install.
Each "[MISS] (REQ)" line has a `hint:` row with the install command.

### `pytest` collection finds 0 tests

You ran pytest from the wrong directory. Either:

```bash
esp-harness test                                        # use the wrapper
# OR
cd tools/esp-harness && python -m pytest tests -v       # use pytest directly
```

`pytest tools/tests` (the old v1.4 path) doesn't exist anymore.

---

## Documentation / discovery issues

### "I don't know which command does what"

```bash
esp-harness manifest --json | jq '.toolkit_commands[].name'
```

Or any specific command's help:

```bash
esp-harness <cmd> --help
```

### "I forgot how the scene framework hooks work"

`components/aurora-harness/README.md` has the full API. The shortest
reference is the [scene_t struct definition](../components/aurora-harness/include/harness/scene_framework.h).

### "I want to add a new feature but don't know where it goes"

`AGENT.md` has a decision table (where things go). Short version:

- **Generic enough that any LVGL+console project would want it** →
  `components/aurora-harness/`
- **Specific to your firmware / Aurora** → your `examples/<name>/main/`
- **A Python tool** → `tools/esp-harness/src/esp_harness/commands/`
- **A reusable host build piece** → `sim-base/`

---

## Reporting a new issue

If your problem isn't in this document, please open an [Issue](https://github.com/Caldis/esp-harness/issues/new?template=bug.md)
with:

- The exact command + the exact output
- `esp-harness --version`
- `esp-harness doctor --json`
- Your OS + ESP-IDF version

Once it's resolved, the maintainer will add an entry here.
