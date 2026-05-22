# FAQ

Common questions answered up-front. If your question isn't here,
check [`troubleshooting.md`](./troubleshooting.md) for symptom-based
fixes, or [open an issue](https://github.com/Caldis/esp-harness/issues/new?template=question.md).

## What is esp-harness?

A monorepo containing four artifacts that together turn an ESP-IDF +
LVGL project into an AI-driveable dev loop:

1. **`aurora-harness`** — a reusable C component (console protocol +
   LVGL scene framework + UI overlays + default `?stat`/`scene`/etc.)
2. **`esp-harness` CLI** — the host-side Python tool (build/flash/sim/
   bench/manifest/etc.)
3. **Aurora** — a reference firmware (20 scenes on a Waveshare board)
4. **Host LVGL simulator** — SDL2-based, runs scenes on your laptop

See [`../README.md`](../README.md) for the full overview and
[`manifesto.md`](./manifesto.md) for the design philosophy.

## Why "harness"?

A test harness is the rigging that lets you exercise a system at speed.
This project is a harness for the ESP-IDF + LVGL system — wired up so
you (or an AI agent) can iterate quickly with structured outputs.

## Do I need the Aurora hardware to use this?

No. Three paths:

| Hardware? | Use |
|---|---|
| You have Waveshare ESP32-S3-Touch-AMOLED-2.16 | Run the Aurora demo (`examples/aurora/`) |
| You have a different ESP32 board | `esp-harness new` + your own BSP (see [`PORTING.md`](../components/aurora-harness/PORTING.md)) |
| No hardware | Use the host sim (`examples/aurora/sim/`) — LVGL on your laptop, no flashing |

The CLI and the component work in all three modes.

## Why not just use ESP-IDF directly?

You can! `idf.py build && idf.py flash && idf.py monitor` is the
substrate. We use it under the hood.

What we add is the **scaffolding above** the raw build:

- Structured device output (`OK:`/`ERR:`/`EVT:` instead of `ESP_LOGI` text scraping)
- One-command discovery of every capability (`manifest --json`)
- A host LVGL build for fast UI iteration
- Visual regression detection (`sim diff`)
- Performance regression detection (`bench --compare`)
- An LVGL scene framework so you don't write the same boilerplate
- Project scaffolding (`esp-harness new`)

If you don't want any of those, raw `idf.py` is fine.

## Why not PlatformIO?

Different layer. PIO is a build wrapper above ESP-IDF (or Arduino).
We're a *testing / scaffolding* wrapper above ESP-IDF.

If we adopted PIO, it would replace `build` / `flash` / `monitor` /
`run` in our CLI — but it would leave everything else untouched
(manifest, sim, bench, scene framework, console protocol). The
remaining 12 commands don't have PIO equivalents.

PlatformIO compatibility as an *additional* path (each example gets a
`platformio.ini`) is plausible future work. See
[`manifesto.md`](./manifesto.md#what-we-dont-do) for the full
rationale.

## Why not Arduino?

Arduino abstracts the layer below where our value lives. We talk
directly to ESP-IDF's component manager, sdkconfig, and BSP layer —
none of that is available in Arduino mode.

If you want to use Arduino for an ESP32 project, that's totally fine;
you just won't need esp-harness.

## Can I use a different display library (not LVGL)?

The scene framework hard-depends on LVGL types (`lv_obj_t`, `lv_color_t`).
The *console protocol* part of `aurora-harness` doesn't — you can use
that in isolation if you only want the AI-driveable serial loop without
LVGL.

For non-LVGL UI frameworks: out of scope. Adding e.g. SquareLine /
slint / your-own-renderer would mean a separate scene framework, which
is essentially a separate project.

## How do I support a new board?

```bash
mkdir boards/<your_board_underscored>/
# write a CMakeLists.txt + idf_component.yml + the LVGL display driver
```

See [`components/aurora-harness/PORTING.md`](../components/aurora-harness/PORTING.md)
for the BSP interface — it's just two functions (`bsp_display_lock` /
`bsp_display_unlock`). If your board has an Espressif-style BSP
component already, you're done; if not, that doc has a shim template.

The Aurora demo's BSP at `boards/esp32_s3_touch_amoled_2_16/` is a
worked example.

## Can I publish my own project that uses aurora-harness?

Yes. The license is MIT — use commercially, include in proprietary
products, no permission needed.

If you're publishing a new ESP-IDF component on top of aurora-harness,
please make it clear in your component's README + idf_component.yml
that it depends on aurora-harness, and pin a specific version.

## How do I keep my project's vendored copy of aurora-harness up to date?

Today, manually. `esp-harness new --component-source vendor` (the
default) copies aurora-harness into your project's `components/`.
There's no auto-update.

To refresh:

```bash
cd your-project/
rm -rf components/aurora-harness
cp -r /path/to/esp-harness/components/aurora-harness ./components/
```

Once aurora-harness is published on the Component Registry, you'll
have a third option: `--component-source depend` writes an
`idf_component.yml` dependency, and `idf.py reconfigure` fetches the
right version.

## What's the difference between `esp-harness new` and `esp-harness init`?

`new` is the v1.5+ command with three vendoring modes (`vendor` /
`link` / `depend`). `init` is the legacy v1.4 alias kept for muscle
memory — it works but doesn't have the new modes. Use `new` for new
code.

## How do I AI-driveable-ify an existing ESP-IDF project?

```bash
# 1. Vendor in the harness component
cp -r /path/to/esp-harness/components/aurora-harness your-project/components/

# 2. In your project's main/CMakeLists.txt:
#    REQUIRES += aurora-harness lvgl__lvgl

# 3. In your app_main:
#    console_protocol_init();
#    harness_default_register();      // ?stat / scene / tap / swipe / ?dump
#    scene_fw_init(lv_screen_active());
#    scene_fw_register(&your_scene);

# 4. Install the toolkit and you can drive the device:
pip install -e /path/to/esp-harness/tools/esp-harness
esp-harness manifest --port COM3
```

That's it — the full AI dev loop is now available against your
existing firmware.

## How does the host simulator compare to the device?

The sim catches:

- ✓ LVGL widget layout bugs
- ✓ Font sizing / overflow
- ✓ Color / theme regressions
- ✓ Scene framework lifecycle issues
- ✓ Logic bugs in pure-UI scenes

The sim **does not** catch:

- ✗ Real peripheral timing
- ✗ PSRAM vs DRAM allocation issues
- ✗ DMA timing / alignment
- ✗ Power management
- ✗ Display driver quirks (e.g. CO5300 SPI underflow)
- ✗ WiFi / BLE coexistence
- ✗ Anything depending on actual hardware behaviour

For UI work the sim is ~80% of the value at 5× the speed. For
peripheral / hardware work, the device build is still required.

## What's "manifest"?

`esp-harness manifest --json` returns a JSON document that enumerates
every capability across all three layers:

```json
{
  "toolkit_commands": [...],   // every esp-harness <cmd>
  "device": {
    "commands": [...],          // every ?xxx the firmware understands
    "scenes": [...]             // every registered scene
  }
}
```

This is the **discovery surface** — the single source of truth an AI
agent reads to know what's possible. If a capability isn't in here,
it doesn't exist for the AI loop.

## What's "doctor"?

`esp-harness doctor` is the environment health check. It verifies:

- ESP-IDF v6+ is installed (via EIM)
- CMake is on PATH (or accessible via EIM)
- Python deps (Pillow, pyserial) are importable
- Optional: MinGW gcc + SDL2 (only needed for sim)
- Optional: a serial port to an ESP32 device
- Optional: the monorepo layout is discoverable

If anything's missing, doctor prints the install command. Failed
required checks → exit 1.

## How do I version-pin my project?

Three places hold version info:

| File | Pins | When to bump |
|---|---|---|
| `components/aurora-harness/idf_component.yml::version` | The component's API contract | When public APIs change |
| `tools/esp-harness/pyproject.toml::version` | The CLI's contract | When CLI subcommands change |
| Repo git tag (`vX.Y.Z`) | The monorepo as a whole | When you cut a release |

Today repo tag and CLI are synced (always — enforced by
`tools/smoke.ps1`'s three-way version case + `tools/release.ps1`'s
sanity gate); the component is on its own track (`1.4.0` last release,
no public-API changes through v1.7 so no bump needed). They may
diverge further as the component / CLI evolve at different paces.

## Can I contribute? How?

Yes — see [`.github/CONTRIBUTING.md`](../.github/CONTRIBUTING.md).
Short version: open an issue first if it's a feature request,
otherwise a PR.

## What's the license?

MIT. See [`LICENSE`](../LICENSE).
