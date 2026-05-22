# AGENT.md — onboarding manual for AI sessions

> If you're an AI dropped into this repo for the first time, read this
> first. The README is for humans; this is for you.

## 0. Bootstrap (first 30 seconds)

```bash
esp-harness manifest --json
```

This single call enumerates everything you can do:
- `toolkit_commands` — every `esp-harness <subcmd>` you can run from the host
- `device.commands` — every console command the firmware exposes (`?help json`)
- `device.scenes` — every registered scene (`scene list`)

**Convention**: if a capability isn't in `manifest`, it doesn't exist for
you. Don't grep firmware source looking for it. If you find something
useful by grepping, *register it* (add it to `?help` and/or
`TOOLKIT_COMMANDS`) so the next session also sees it.

## 1. The three layers

```
┌──────────────────────────────────────────────────────────┐
│                                                          │
│   esp32-harness-toolkit          (sibling repo)          │
│   ──────────────────                                     │
│   Python CLI: build / flash / monitor / console / sim    │
│   diff / record / bench / manifest / init                │
│                                                          │
└─────────────────┬────────────────────────────────────────┘
                  │ subprocess + JSON
                  ▼
┌──────────────────────────────────────────────────────────┐
│                                                          │
│   esp32-harness-showcase                                 │
│   ──────────────────                                     │
│   ├── components/aurora-harness/      reusable scaffold  │
│   │      console_protocol + scene_framework +            │
│   │      toast + default ?stat                           │
│   ├── main/                           Aurora-specific:   │
│   │      aurora_main + 20 scenes + peripheral commands   │
│   └── sim/                            host LVGL build:   │
│          SDL2 entry + ESP-IDF stubs + mock peripherals   │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

The **toolkit** is your hands. The **firmware** is the system under
test. The **simulator** is the fast iteration surface for pure-UI
changes.

## 2. Where to put things

| Doing this... | Goes in... | Why |
|---|---|---|
| Generic console command (works in any LVGL project) | `components/aurora-harness/src/default_cmds.c` | Reusable in other projects via the component |
| Peripheral-coupled command (?audio, ?sd, wifi, etc.) | `main/harness/harness_commands.c` | Aurora-specific, depends on `main/peripherals/*` |
| New scene with new behavior | `main/scenes/scene_<name>.c` + register in `aurora_main.c` + add to `sim/main.c` if host-runnable | Standard scene wiring |
| Reusable UI primitive (toast, modal, picker) | `components/aurora-harness/src/<primitive>.c` + header at `include/harness/` | These are the LEGO blocks, lift them |
| New peripheral driver | `main/peripherals/<name>.{c,h}` | Aurora has its peripherals, your project will too |
| Host-side mock for a peripheral | `sim/mock_peripherals.c` | Project-specific — write the smallest plausible behavior |
| Toolkit command | `esp32-harness-toolkit/src/esp_harness/commands/<name>.py` | Wire into `cli.py` + `manifest.py::TOOLKIT_COMMANDS` |

## 3. Standard workflows

### Make a UI change

```bash
# 1. edit main/scenes/scene_foo.c (or main/ui_shell.c, etc)
# 2. confirm sim still passes
cmake --build sim/build -j                                  # ~3 s
esp-harness sim diff --scenes foo,grid,bloom                # ~5 s
# 3. if you changed something intentionally that broke a golden:
esp-harness sim update-golden --scenes foo
# 4. only NOW build firmware (slow):
esp-harness build && esp-harness flash
```

### Add a console command

```c
// in main/harness/harness_commands.c (for peripheral-coupled cmds)
// or components/aurora-harness/src/default_cmds.c (for generic cmds):

static int cmd_my(const console_args_t *a) {
    console_reply_ok("did the thing");
    return 0;
}
static const console_cmd_t s_cmd_my = { "?my", cmd_my, "do my thing" };

void register_my(void) {
    console_protocol_register(&s_cmd_my);
}
```

`?help json` will auto-surface it. Update toolkit `TOOLKIT_COMMANDS`
if it's worth highlighting in the manifest.

### Investigate a panic

```bash
esp-harness backtrace --monitor 30 --json
```

addr2line runs automatically against `build/aurora.elf`.

### Cross-version performance check

```bash
# After a known-good firmware build:
esp-harness bench --baseline --json

# Later, after a change:
esp-harness bench --compare --json
# exit 1 + diff details if any metric breached its threshold
```

## 4. Discovery surface invariants

**Promise**: every capability is reachable through `manifest`.

The wires:
- Every `console_cmd_t` registered with `console_protocol_register()`
  appears in `?help` (text mode) and `?help json` (machine-readable).
- Every `scene_t` registered with `scene_fw_register()` appears in
  `scene list` (JSON manifest, payload-framed).
- Every toolkit subcommand must be in `commands/manifest.py::TOOLKIT_COMMANDS`
  — CI lint (`tools/check_manifest.py`) fails if you forget.

So when you add a new thing, you don't need to update three docs —
the runtime sources are the docs.

## 5. Things that look like bugs but aren't

| Symptom | Reality |
|---|---|
| `bsp_display_lock(0)` deadlock on busy frames | Use `bsp_display_lock(-1)` from non-LVGL tasks. 0 is non-blocking try-lock which silently fails. |
| `scene_fw_register` past `MAX_SCENES` silently drops | Bumped to 24 with `ESP_LOGE` on overflow in v1.1. If you need more, bump it. |
| WiFi scan returns -1 after BLE was up | Same internal-SRAM pool. First scene to need WiFi calls `ble_deinit()` to free it; afterwards BLE returns -1 until reboot. Hardware constraint. |
| `esp_codec_dev_read` returns 0 on success | Yes really — see `peripherals/audio.c`. |
| Boot shows scene 0 briefly then jumps to last scene | Pre-v1.0 bug. Now `settings_init` + `last_scene` restore happen before LVGL unlock — no flicker. |
| `?audio loopback 500` returns elapsed_ms = 1057 | The 500 is the *capture* duration; loopback also plays back, so total is ~2× plus codec setup. Not a bug. |
| `cmd_dump` allocates 434KB in PSRAM the first call | Intentional. Reused forever. Allocating per-call wedges the device. |

## 6. What lives where

```
showcase/
├── README.md              human-facing overview
├── AGENT.md               you are here
├── CHANGELOG.md           version-tagged changes
├── KNOWN_ISSUES.md        accepted limitations
├── docs/
│   ├── scenes-map.md      18 scenes -> peripherals table
│   └── harness-report.html visual progress report
├── components/
│   ├── aurora-harness/    <-- reusable core (you should READ its README)
│   └── esp32_s3_touch_amoled_2_16/   vendored BSP w/ small patches
├── main/                  Aurora app code
│   ├── aurora_main.c      app_main wiring
│   ├── ui_shell.c         chrome (dots, name label)
│   ├── harness/           harness_commands.c — Aurora-specific console
│   ├── peripherals/       imu/pmic/audio/sd/wifi/ble/keys/system/settings
│   └── scenes/            19 scene .c files
└── sim/                   host LVGL build (see sim/README + INTEGRATION)
    ├── main.c             SDL2 + scene_fw entry
    ├── mock_*.{c,h}       host stubs for BSP + peripherals
    ├── include/           ESP-IDF stub headers (esp_log/freertos/...)
    └── golden/            visual regression baseline BMPs
```

## 7. Cardinal rule

**Don't break the manifest.**

The whole AI loop assumes `esp-harness manifest --json` returns a
complete, accurate inventory. If you add a thing without registering it,
the next session can't see it and will re-discover it the hard way.

If you find yourself thinking "this works without registering, I'll
register later" — register *now*. The manifest is the only honest
source of truth.
