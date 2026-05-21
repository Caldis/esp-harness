# Architecture

How the three layers fit together and why they were drawn this way.

## The three layers

```
                              ┌────────────────────────────────────┐
                              │  HOST (laptop / CI)                │
       Layer 1                │  ────────────────                  │
       Toolkit                │  tools/esp-harness/                │
                              │  Python CLI: build / flash / sim / │
                              │  bench / manifest / doctor / new   │
                              │                                    │
                              └────────────────┬───────────────────┘
                                               │ subprocess + JSON
                                               │ + serial line protocol
                                               ▼
                              ┌────────────────────────────────────┐
                              │  DEVICE / SIMULATOR                │
       Layer 2                │  ─────────────────                 │
       Component              │  components/aurora-harness/        │
       (REUSABLE)             │                                    │
                              │  Console protocol (OK / ERR / EVT) │
                              │  Scene framework (LVGL deck)       │
                              │  Toast + progress overlays         │
                              │  Default cmds: ?stat / scene /     │
                              │     tap / swipe / ?dump            │
                              │  BSP interface (lock/unlock only)  │
                              │                                    │
                              └────────────────┬───────────────────┘
                                               │ uses (consumer side)
                                               ▼
                              ┌────────────────────────────────────┐
                              │                                    │
       Layer 3                │  examples/<name>/                  │
       Application            │  ──────────────                    │
       (per-project)          │  app_main + scenes + peripherals   │
                              │  + app-specific console cmds       │
                              │  + (optional) host sim with mocks  │
                              │                                    │
                              └────────────────────────────────────┘
```

Each layer talks down to the one below it. Layer 1 (toolkit) is
host-side Python; Layer 2 (component) is on-device C; Layer 3 (app)
is per-project C using Layer 2.

## Why these boundaries

### Layer 1 — toolkit

**Job**: drive the device without writing serial boilerplate every time.

**Stays out of**: anything firmware-specific. The toolkit knows nothing
about Aurora's scenes, audio commands, or peripherals — it just sends
lines and parses `OK:` / `ERR:` / `EVT:` framing. Anything device-
specific lives in the manifest fetched at runtime (`?help json` +
`scene list`).

This means a different consumer (someone else's project using
aurora-harness) gets the same `esp-harness console`, `esp-harness sim
diff`, `esp-harness bench compare` workflows for free.

### Layer 2 — component (the library)

**Job**: provide the universally-useful scaffolding every LVGL +
console project re-invents.

**Includes**: console protocol, scene framework, toast / progress
overlays, default commands that any project would want (`?stat` for
heap / fps, `scene` for navigation, `tap` / `swipe` / `?dump` for
remote interaction).

**Excludes**: peripheral drivers (board-specific), business logic
(app-specific), UI chrome layers like ui_shell (app-specific decoration).
The component touches LVGL primitives but never imports `peripherals/imu.h`
or `ui_shell.h` — those don't exist in someone else's project.

The single coupling point to the board is `harness/bsp_iface.h`, which
declares two functions: `bsp_display_lock(timeout_ms)` and
`bsp_display_unlock()`. Every Espressif-style BSP supplies these. See
[`components/aurora-harness/PORTING.md`](../components/aurora-harness/PORTING.md).

### Layer 3 — application (per-project)

**Job**: be a useful thing on its own (Aurora is generative art; yours
might be a smart-home controller, a watch face, an instrument cluster).

**Owns**: `app_main`, peripheral drivers, scenes specific to the app,
app-specific console commands (`?audio`, `?sd`, `?wifi`, etc. in Aurora's
case), the UI chrome (status bar, mode indicators), persistence (NVS
keys for app settings).

**Uses Layer 2 by**: `EXTRA_COMPONENT_DIRS` for path-based vendoring,
`REQUIRES aurora-harness` in main/CMakeLists.txt, registering a
scene change listener (`scene_fw_set_change_listener`) to update UI
chrome on scene transitions.

## The discovery surface

A single command flattens everything across all three layers:

```bash
esp-harness manifest --json
# returns:
#   toolkit_commands      ← Layer 1 inventory
#   device.commands       ← Layer 2 + Layer 3 console commands (via ?help json)
#   device.scenes         ← Layer 2 + Layer 3 scenes (via scene list)
```

This is the single source of truth for "what can I do right now".
Any new capability that's not in here doesn't exist for the AI driving
the loop.

## The simulator (sim-base + per-example sim/)

The host simulator lets you iterate on UI changes in ~5 s (vs ~30 s to
flash a device). It's a parallel build of the scene framework + LVGL
running on SDL2.

```
                    sim-base/                     examples/aurora/sim/
                    (generic, board-agnostic)     (Aurora-specific)
                    ─────────────────────         ───────────────────
                    ESP-IDF stubs (esp_log,       lv_conf.h (Aurora's
                      esp_err, esp_timer, ...)      LVGL config)
                    mock_bsp.c (lock no-op)       main.c (registers 13
                    tools/setup.ps1                 Aurora scenes)
                                                  mock_peripherals.c
                                                    (Aurora's IMU/PMIC/
                                                    keys signatures)
                                                  golden/*.bmp (visual
                                                    regression baseline)
                                                  CMakeLists.txt (assembles
                                                    sim-base + Aurora-specific
                                                    + LVGL into aurora_sim)
```

`sim-base/` is intentionally the smaller half. Adopting the sim
template for your own project means copying the `examples/aurora/sim/`
shape and substituting your scene list + your peripheral mocks. See
[`sim-base/INTEGRATION.md`](../sim-base/INTEGRATION.md).

## Why these particular tradeoffs

- **One repo, multiple artifacts**: Easier to keep aurora-harness and
  the Aurora demo from drifting (they were two separate repos in v1.3–
  v1.4 and drift was a constant minor pain).
- **Components as ESP-IDF components**: We use ESP-IDF's component
  discovery rather than a custom build system. `EXTRA_COMPONENT_DIRS`
  points at `<root>/components` and `<root>/boards`; ESP-IDF discovers
  them by directory name.
- **Toolkit as pip-installable**: Anyone with Python can `pip install
  -e tools/esp-harness/` without needing the whole repo. We may
  eventually publish to PyPI.
- **Sim as a host build**: SDL2 + LVGL on the host means no Xtensa-
  specific quirks during UI iteration. Sim diff catches widget-tree
  regressions even when nothing about peripherals changed.
- **AI-driven contract**: Every interface (console JSON, manifest,
  exit codes, payload framing) is parseable by a script without
  human eyeballing.

## What this architecture does not address

- **Performance**: Sim numbers mean nothing on-device. Real timing /
  PSRAM behavior / DMA quirks only show up on hardware.
- **Power management**: Sleep modes, brownout, OTA partition swaps
  are not in scope for the simulator. Test on hardware.
- **Multi-device orchestration**: One device per `esp-harness` invocation
  today. Adding a daemon for orchestration would be a v2.0 conversation.

See [`harness-report.html`](./harness-report.html) for a visual
walkthrough of how this architecture evolved across v1.0–v1.5.
