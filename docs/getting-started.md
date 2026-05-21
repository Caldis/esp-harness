# Getting started

Concrete first-15-minutes paths for the three most common entry points.

---

## Path A — "I want to use aurora-harness in my own ESP-IDF project"

This is the most common path. You're starting a new ESP-IDF + LVGL
project and want the console / scene / overlay primitives for free.

### Prerequisites

- ESP-IDF v6.0+ installed (via [EIM](https://docs.espressif.com/projects/idf-im-ui/))
- Python 3.10+
- An ESP32-family board with an LVGL-compatible display

### Step 1: Install the toolkit

```bash
git clone https://github.com/Caldis/esp-harness
cd esp-harness
pip install -e tools/esp-harness/
esp-harness doctor                  # verify env health
```

### Step 2: Scaffold your project

```bash
esp-harness new my-thing --component-source vendor
cd my-thing
```

The `vendor` source copies `aurora-harness/` into your project's
`components/`. Your project is self-contained — you can delete the
esp-harness clone afterwards and your project still builds.

Alternatives:
- `--component-source link` keeps a sibling-repo reference (lighter
  footprint, requires the esp-harness clone to stay)
- `--component-source depend` writes an `idf_component.yml` dependency
  (TBD — requires aurora-harness to be published to the Component
  Registry first)

### Step 3: Build + flash

```bash
esp-harness build && esp-harness flash
esp-harness monitor --seconds 4 --until "ready"
```

You should see "I (xxxx) my_thing: ready, 1 scene(s)" in the monitor
output, and a "Hello" label on your board's screen.

### Step 4: Customise

Look at `main/scenes/scene_hello.c` — that's the minimal scene template
you start from. Copy it, rename, edit, then add to `main/CMakeLists.txt`
SRCS + `main/scenes/scenes.h` + register in `main/<name>_main.c`.

For everything aurora-harness gives you, read
[`components/aurora-harness/README.md`](../components/aurora-harness/README.md).
The 5-line quickstart at the top covers 80%.

---

## Path B — "I want to run the Aurora demo"

You have a [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm)
and want to see what the demo does.

### Prerequisites

- ESP-IDF v6.0+ (via EIM)
- Python 3.10+
- The Waveshare board + USB-C cable

### Steps

```bash
git clone https://github.com/Caldis/esp-harness
cd esp-harness
pip install -e tools/esp-harness/

cd examples/aurora
esp-harness build && esp-harness flash
```

After flashing you should see Aurora's 20 scenes cycling on tap.
[`examples/aurora/README.md`](../examples/aurora/README.md) walks
through the scene roster, the console commands (`?audio`, `?sd`,
`?wifi`, etc.), and known limitations.

To explore the scene framework without hardware, jump to Path C.

---

## Path C — "I want the host simulator (no hardware)"

Run a subset of Aurora's scenes on your laptop via SDL2 + LVGL. ~5 s
build / iteration cycle vs ~30 s flash.

### Prerequisites

- **Windows**: MinGW gcc + SDL2 dev libs (the `esp-harness doctor`
  command tells you exactly what's missing + how to install — typically
  via [scoop](https://scoop.sh/) or chocolatey)
- **Linux**: `sudo apt install build-essential libsdl2-dev cmake`
- **macOS**: `brew install sdl2 cmake`
- Plus the toolkit installed (Path A's Step 1).

### Steps

```bash
cd esp-harness
esp-harness doctor                   # checks for sdl2 / mingw / pillow
                                     # prints install commands for anything
                                     # missing

# First-time only: populate managed_components/lvgl__lvgl/ in Aurora
cd examples/aurora
idf.py reconfigure
                                     # ^ pulls LVGL via component manager
                                     # (alternatively: pass -DLVGL_DIR= when
                                     #  configuring the sim build)

# Build the sim
cd sim
cmake -B build [-DSDL2_PREFIX=path]  # SDL2_PREFIX needed on Windows
cmake --build build
./build/aurora_sim                   # opens a 466x466 window

# Headless mode (for CI / scripted snapshots):
./build/aurora_sim --scene 0 --exit-after-ms 600 --snapshot out.bmp
esp-harness sim diff                 # compares 13 scenes to golden baseline
```

For the full sim workflow including `update-golden`, `record`,
per-scene tolerances, see [`examples/aurora/sim/README.md`](../examples/aurora/sim/README.md).

---

## Repo conventions

Just enough to be useful immediately; the full pedagogy is in
[`AGENT.md`](../AGENT.md).

### Versioning

- **Repo-level** (e.g. `v1.5.0`) is tagged on every milestone milestone.
- **Component-level** (`components/aurora-harness/idf_component.yml`)
  follows the same number — they're released together for now.
- **Toolkit-level** (`tools/esp-harness/pyproject.toml`) is independent
  semver — bumps when the CLI surface changes.

### Branches

- `master` — release-ready trunk. Tags cut from here.
- Feature branches: `feat/<short-name>`, `fix/<short-name>`,
  `docs/<short-name>`. Open a PR to merge.

### Tests

Three layers of validation, all wired into the CI workflow:

```bash
esp-harness test                     # runs pytest tools/esp-harness/tests/
                                     # = 3 integration tests:
                                     #   - doctor health
                                     #   - manifest completeness
                                     #   - sim diff (13 scenes vs golden)
```

CI runs this on every push touching the relevant paths.

### Where docs live

- **Root README.md**: three-path onboarding funnel + one-screen overview.
- **Root AGENT.md**: cross-cutting AI-onboarding manual.
- **Each artifact's own README.md**: that artifact's quickstart + API +
  limits. Component README is the longest; per-example READMEs are
  short.
- **`docs/`**: only cross-artifact concerns (architecture, this file,
  the progress report).

---

## Common questions

### "Do I need to clone the whole repo to use aurora-harness?"

No, if you used `esp-harness new --component-source vendor` (the
default). Your project gets a copy in its `components/`. Once we
publish to the Component Registry, `--component-source depend` will
also work without any local clone.

### "Can I add a different board?"

Yes. Add `boards/<your_board_underscored>/` with the same shape as
`boards/esp32_s3_touch_amoled_2_16/`. Your project's `main/CMakeLists.txt`
REQUIRES it. See [`components/aurora-harness/PORTING.md`](../components/aurora-harness/PORTING.md)
for the BSP interface.

### "Does this work without an LVGL display?"

The console protocol (just `?ping` / `?reset` / your own commands)
works without LVGL. The scene framework needs LVGL. There's no
hard requirement to use both — call `console_protocol_init()` alone
if you only want the protocol.

### "What about PlatformIO?"

Not used today; we drive ESP-IDF directly via `idf.py`. Adding a
`platformio.ini` to an example so PIO users can build it is on the
roadmap but not urgent.

### "How do I report a bug?"

Open an issue at [github.com/Caldis/esp-harness/issues](https://github.com/Caldis/esp-harness/issues).
Include the output of `esp-harness doctor` and `esp-harness manifest`.
