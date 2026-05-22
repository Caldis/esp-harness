<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/brand/logo-dark.svg">
  <img src="docs/brand/logo.svg" alt="esp-harness" width="120" height="120">
</picture>

# esp-harness

**AI-driven dev-loop scaffold for ESP-IDF + LVGL projects.**

[![CI](https://img.shields.io/github/actions/workflow/status/Caldis/esp-harness/sim-diff.yml?branch=master&label=sim+diff)](https://github.com/Caldis/esp-harness/actions/workflows/sim-diff.yml)
[![release](https://img.shields.io/github/v/tag/Caldis/esp-harness?label=release&color=b8431a)](https://github.com/Caldis/esp-harness/releases)
[![license](https://img.shields.io/github/license/Caldis/esp-harness?color=344a36)](./LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0%2B-1c1814)](https://docs.espressif.com/projects/esp-idf/)
[![LVGL](https://img.shields.io/badge/LVGL-9.x-1c1814)](https://lvgl.io/)
[![docs](https://img.shields.io/badge/docs-homepage-b8431a)](https://caldis.github.io/esp-harness/)
[![contributions](https://img.shields.io/badge/contributions-welcome-b8431a)](./.github/CONTRIBUTING.md)

[**Quickstart**](#quickstart-30-seconds) ·
[**What is this**](#what-this-is) ·
[**Why**](#why-it-exists) ·
[**Docs**](./docs/) ·
[**Manifesto**](./docs/manifesto.md) ·
[**Aurora demo**](./examples/aurora/) ·
[**Toolkit CLI**](./tools/esp-harness/)

</div>

---

## What this is

A reusable C component + a Python CLI + a reference firmware + a desktop
simulator, all in one monorepo, designed so an AI agent (or a human) can
iterate on ESP32 firmware **5× faster than a normal `idf.py monitor`
loop** by removing the parts that don't deserve attention.

You get:

| Layer | What it gives you |
|---|---|
| 🪛 **`aurora-harness`** C component | Line-protocol console (`OK:` / `ERR:` / `EVT:` + payload framing) · LVGL scene framework · toast / progress overlays · default `?stat` / `scene` / `tap` / `swipe` / `?dump` commands. Drop into any ESP-IDF + LVGL project. |
| 🐍 **`esp-harness`** Python CLI | One discovery surface (`manifest --json` enumerates everything). Build / flash / monitor / sim diff / bench compare / scaffold new project / env doctor / pytest integration. |
| 🌌 **Aurora** reference firmware | 20 scenes exercising every onboard peripheral of the Waveshare ESP32-S3-Touch-AMOLED-2.16. The proof, the worked example, the visual regression baseline. |
| 🖥️ **Host LVGL simulator** | Pure-UI iteration in ~5 s on your laptop, no flashing. Visual regression via `sim diff` on every PR. |

## Quickstart (30 seconds)

```bash
git clone https://github.com/Caldis/esp-harness
cd esp-harness
pip install -e tools/esp-harness/        # if `esp-harness` isn't on PATH after,
                                          # the Scripts/bin dir of your Python
                                          # isn't on PATH — `python -m esp_harness`
                                          # always works as a fallback.
esp-harness doctor                       # what's healthy in your env?
esp-harness new my-thing                 # scaffolds a fresh project (default:
                                          # --component-source link — wires the
                                          # Waveshare BSP automatically)
cd my-thing && esp-harness build         # builds it (≈ 60s clean)
```

That's it. Your `my-thing/` directory now contains an ESP-IDF + LVGL
project that **builds out of the box** on the Waveshare ESP32-S3
AMOLED 2.16 board — a "Hello" scene rendering, the console protocol
live, the screenshot pipeline ready.

> **Targeting a different board?** The default scaffolds for the Aurora
> hardware. Pass `--component-source vendor` for a self-contained copy
> + manually wire your BSP into `main/CMakeLists.txt`, or see the
> generated `README.md`'s "Before your first build" section.

> **Windows + clean Python install?** `pip install -e tools/esp-harness/`
> may put the `esp-harness` shim at a path that isn't on `PATH` by
> default (e.g. `…\AppData\Roaming\Python\Python3xx\Scripts\`). Two
> equivalent fixes:
> - run `tools/esp-harness/install.ps1` — creates a dedicated venv,
>   adds a PowerShell-profile shim function so `esp-harness` resolves
>   in a new shell.
> - or use `python -m esp_harness <command>` always (works regardless
>   of `PATH`).

For the full setup (flashing to hardware, running the simulator, etc.)
see [`docs/getting-started.md`](./docs/getting-started.md).

## Why it exists

The default ESP-IDF dev loop is **edit → 30 s flash → scroll log →
hope**. That's fine for one-off hacks; it's brutal when you want a
serious feedback loop or an AI agent in the chair.

esp-harness exists to solve three specific problems:

1. **Every device output is parseable.** No more grepping `ESP_LOGI`
   lines for evidence the change worked. `OK:` / `ERR:` / `EVT:`
   framing means a host script (or AI) can act on results immediately.

2. **Discovery is one command.** `esp-harness manifest --json` returns
   a complete inventory of every capability. If it's not in there, it
   doesn't exist for the AI. No more "go read the docs to find the
   feature".

3. **UI iteration runs on the host.** The simulator at `sim-base/` +
   per-example `sim/` builds the same scenes against LVGL/SDL2 on
   your laptop in ~5 s. Visual regression catches breakage on every
   PR.

See [`docs/manifesto.md`](./docs/manifesto.md) for the full design
rationale.

## Three onboarding paths

Pick the one that matches your goal:

### 🧱 "I want to USE aurora-harness in my own project"

```bash
pip install -e tools/esp-harness/
esp-harness new my-thing                 # default: link mode (BSP auto-wired);
                                          # use --component-source vendor for a
                                          # self-contained copy
cd my-thing
esp-harness build && esp-harness flash
```

→ [`components/aurora-harness/README.md`](./components/aurora-harness/README.md) (full API)
→ [`components/aurora-harness/PORTING.md`](./components/aurora-harness/PORTING.md) (other boards)

### 🌌 "I want to RUN the Aurora demo on my Waveshare board"

```bash
cd examples/aurora/
esp-harness build && esp-harness flash
esp-harness monitor --seconds 8 --until "ready"
```

→ [`examples/aurora/README.md`](./examples/aurora/README.md) (the 20 scenes, the console commands, the limitations)

### 🖥️ "I want to TRY the host simulator (no hardware)"

```bash
esp-harness doctor                       # checks SDL2 + MinGW + Pillow
cd examples/aurora/sim/
cmake -B build && cmake --build build
esp-harness sim diff                     # 13 scenes vs golden baseline
```

→ [`examples/aurora/sim/README.md`](./examples/aurora/sim/README.md) (sim-specific setup)
→ [`sim-base/INTEGRATION.md`](./sim-base/INTEGRATION.md) (adopt the sim template)

### 🤝 "I'm contributing"

→ [`docs/architecture.md`](./docs/architecture.md) (the 3-layer mental model)
→ [`docs/manifesto.md`](./docs/manifesto.md) (the design philosophy)
→ [`AGENT.md`](./AGENT.md) (for AI sessions specifically)
→ [`.github/CONTRIBUTING.md`](./.github/CONTRIBUTING.md) (PR workflow)

## How it compares

Different tools target different layers. Here's where esp-harness
sits:

| Tool / Stack | What it gives you | What you still need from esp-harness |
|---|---|---|
| **Raw ESP-IDF + `idf.py`** | Build / flash / monitor. Mature. Official. | Everything above the wire: AI loop, manifest, sim, structured console, bench, scene framework |
| **PlatformIO** | Build / flash / monitor with cross-platform abstraction. VS Code integration. Library registry. | Same as above — PIO is a fancier build wrapper, not an AI dev loop. |
| **Arduino IDE for ESP32** | One-button build / upload. Easy first-touch. | Mostly everything — Arduino abstracts the layer below our value-add. |
| **LVGL official sim** | Host LVGL build template. No ESP-IDF awareness. | The bridge from your firmware code to the sim (mock_bsp, stubs, scene framework) |
| **esp-harness (this repo)** | All of the above's value-add, plus the AI dev loop. Targeted at ESP-IDF + LVGL specifically. | — |

We don't replace ESP-IDF (we use it). We don't replace LVGL (we use
it). We don't compete with PlatformIO (we coexist; if you want a
`platformio.ini` for an example, open an issue). What we add is the
**scaffolding around** these tools that an AI / CI / serious testing
loop needs.

## Status

| | |
|---|---|
| **Current release** | `v1.7.4` (2026-05-22) — round-5 falsification convergence. Pre-release [smoke gate](./tools/smoke.ps1) at 22/22 (7 host + 15 device). See [`CHANGELOG.md`](./CHANGELOG.md). Release process: [`RELEASING.md`](./RELEASING.md). |
| **Hardware tested** | Waveshare ESP32-S3-Touch-AMOLED-2.16 (Aurora demo). Other boards: bring-up clear from [`PORTING.md`](./components/aurora-harness/PORTING.md), PRs welcomed. |
| **Component Registry** | aurora-harness `idf_component.yml` is registry-ready; published version pending. |
| **CI** | GitHub Actions on every push / PR — sim diff against 13 host-renderable scenes (of 20 total) + 3 integration tests. |
| **License** | MIT. |
| **Predecessors** | [`esp32-harness-showcase`](https://github.com/Caldis/esp32-harness-showcase) (v1.0-v1.4) and [`esp32-harness-toolkit`](https://github.com/Caldis/esp32-harness-toolkit) (v1.0) — kept as archives. |

See [`CHANGELOG.md`](./CHANGELOG.md) for the full release history,
[`docs/harness-report.html`](./docs/harness-report.html) for the
visual progress report.

---

<div align="center">

Made with intent, in pursuit of <em>shorter loops</em>.

</div>
