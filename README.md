# esp-harness

> An AI-driven dev-loop scaffold for ESP-IDF + LVGL projects.
> One reusable C component + one host-side CLI + a runnable reference
> firmware + a desktop simulator — all in one monorepo, designed so an
> AI agent can iterate on UI / firmware faster than a human can scrub
> through `idf.py monitor`.

```
clone → install toolkit → run the demo, OR scaffold your own:

    git clone https://github.com/Caldis/esp-harness
    cd esp-harness
    pip install -e tools/esp-harness/
    esp-harness new my-thing            # ← starts a fresh project
    cd my-thing && esp-harness build && esp-harness flash
```

---

## What's in this repo

Five top-level directories, each one type of artifact:

```
esp-harness/
├── components/aurora-harness/      ← reusable C component (the LIBRARY)
│       Drop into any ESP-IDF + LVGL project for: line-protocol console
│       (OK / ERR / EVT + payload framing), LVGL scene framework, toast +
│       progress overlays, default ?stat / scene / tap / swipe / ?dump.
│
├── tools/esp-harness/              ← Python CLI (the TOOLKIT)
│       The host side of the dev loop: build / flash / monitor / sim diff
│       / bench compare / manifest / doctor / test / new (scaffolder).
│       Installable via `pip install -e tools/esp-harness/`.
│
├── examples/aurora/                ← reference firmware (the DEMO)
│       Generative-art companion for the Waveshare ESP32-S3-Touch-AMOLED-
│       2.16. 20 scenes exercising every onboard peripheral. The reference
│       implementation of "how to use aurora-harness".
│
├── sim-base/                       ← host LVGL build template
│       The board-agnostic half of running aurora-harness on your laptop:
│       ESP-IDF stub headers, mock_bsp, SDL2 setup. Per-project pieces
│       (scene list, peripheral mocks) live in each example's sim/.
│
├── boards/esp32_s3_touch_amoled_2_16/  ← vendored Waveshare BSP
│       Patched for ESP-IDF v6.0 compatibility. Auto-discovered by Aurora.
│       Future boards drop in alongside.
│
└── docs/                            ← cross-cutting docs
        architecture.md   getting-started.md   harness-report.html
```

---

## Three onboarding paths

Pick the one that matches your goal:

### ▸ "I want to USE aurora-harness in my own ESP-IDF project"

```bash
pip install -e tools/esp-harness/
esp-harness new my-thing --component-source vendor
cd my-thing
esp-harness build && esp-harness flash
```

`--component-source vendor` (default) copies `aurora-harness/` into your
new project's `components/`. Your project is self-contained.

Read [`components/aurora-harness/README.md`](./components/aurora-harness/README.md)
for the API contract — the 5-line quickstart at the top is enough to get
going.

### ▸ "I want to RUN the Aurora demo on my Waveshare board"

```bash
pip install -e tools/esp-harness/
cd examples/aurora/
esp-harness build && esp-harness flash
esp-harness monitor --seconds 8 --until "ready"
```

You'll need a [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm)
and a USB-C cable. See [`examples/aurora/README.md`](./examples/aurora/README.md)
for the full demo walkthrough, the 20 scenes, and known limitations.

### ▸ "I want to TRY the host simulator (no hardware needed)"

```bash
pip install -e tools/esp-harness/
esp-harness doctor                   # checks SDL2 + MinGW + Pillow etc.
cd examples/aurora/sim/
cmake -B build -DLVGL_DIR=...        # see sim/README.md for the full setup
cmake --build build
esp-harness sim diff                  # runs the 13 host-portable scenes
```

See [`examples/aurora/sim/README.md`](./examples/aurora/sim/README.md)
for the Aurora-specific setup, or [`sim-base/INTEGRATION.md`](./sim-base/INTEGRATION.md)
for adapting the sim template to your own project.

### ▸ "I'm contributing / understanding the architecture"

Start at [`docs/architecture.md`](./docs/architecture.md) for the 3-layer
mental model, then [`docs/getting-started.md`](./docs/getting-started.md)
for repo conventions, then [`AGENT.md`](./AGENT.md) for the AI-session
onboarding.

---

## What this is, in one paragraph

`aurora-harness` solves the same problem every ESP32 + LVGL project hits:
"how do I drive my device from a host script (or AI) without writing the
serial dance from scratch every time?" The C component gives you a line-
protocol console (`OK:` / `ERR:` / `EVT:`) + a scene framework (deck of
full-screen LVGL views with lifecycle hooks). The toolkit gives you the
host CLI to talk to it. Together they form a closed AI dev loop: write
code → `esp-harness run` → parse JSON → adjust → repeat, no manual serial
debugging.

The Aurora demo is the proof. The sim is the speed boost (5 s host
iteration instead of 30 s flash). The `init` template is the on-ramp.

---

## What this isn't

- **Not a board support package.** `boards/` only contains what we
  vendor for the Aurora demo. Your own project supplies its own BSP.
- **Not a GUI framework.** LVGL is the GUI framework. aurora-harness is
  scaffolding around it.
- **Not a replacement for ESP-IDF or PlatformIO.** It runs alongside
  ESP-IDF (uses `idf.py` under the hood). PlatformIO isn't used; could
  be added by writing `platformio.ini` for an example.
- **Not committed to backwards compatibility across major versions.**
  Semver is followed; major bumps (2.0, 3.0) may rename / remove APIs.

---

## Status

- **v1.5.0** (2026-05-21) — monorepo migration. Predecessors:
  - `esp32-harness-showcase` v1.4.0 — kept as archive
  - `esp32-harness-toolkit` v1.0.0 — kept as archive
- **License**: MIT
- **CI**: GitHub Actions on every push / PR (visual regression + integration tests)

See [`CHANGELOG.md`](./CHANGELOG.md) for the full release history and
[`docs/harness-report.html`](./docs/harness-report.html) for the visual
progress report.
