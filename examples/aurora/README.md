# Aurora — reference firmware

[![example](https://img.shields.io/badge/example-aurora-b8431a)](./CMakeLists.txt)
[![scenes](https://img.shields.io/badge/scenes-20-1c1814)](./main/scenes/)
[![board](https://img.shields.io/badge/board-Waveshare_ESP32--S3--Touch--AMOLED--2.16-1c1814)](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm)
[![sim](https://img.shields.io/badge/sim-13_scenes_host--runnable-344a36)](./sim/)
[![harness](https://img.shields.io/badge/aurora--harness-vendored-b8431a)](../../components/aurora-harness/)

A generative-art companion piece for the **Waveshare ESP32-S3-Touch-AMOLED-2.16**
(466 × 466 round AMOLED, CO5300 driver, CST9217 touch) — and the
**reference firmware** for the esp-harness monorepo. 20 scenes
exercising every onboard peripheral. Built end-to-end by an AI agent —
no human typed any C in this repo.

→ Part of the [esp-harness monorepo](../../README.md). For the full
ecosystem context see the [root README](../../README.md).

This example uses:

- **`components/aurora-harness/`** — the C library (console + scene framework + overlays + default cmds)
- **`tools/esp-harness/`** — the host CLI to build / flash / monitor / sim
- **`boards/esp32_s3_touch_amoled_2_16/`** — the Waveshare BSP (auto-resolved)
- **`sim-base/`** — the host LVGL build infrastructure (mock_bsp + ESP-IDF stubs)

## What's in the box (v1.2)

This repo is three things at once:

1. **Aurora the firmware** — 18 generative-art / diagnostic scenes
   exercising every onboard peripheral of the Waveshare board. The
   demo / proof.
2. **`components/aurora-harness/`** — reusable ESP-IDF component
   containing the line-protocol console (`?help` / `?reset` /
   `OK:` / `ERR:` / `EVT:` / payload framing) and the LVGL scene
   framework. **Drop this into your own project** to get the same
   AI-driveable dev loop. See [its README](../../components/aurora-harness/README.md).
3. **`sim/`** — host-side LVGL build under SDL2. Runs 13 of the 20
   scenes on a Windows / Linux / macOS desktop without flashing,
   enabling 5-second UI iterations + visual regression via
   `esp-harness sim diff`. See [`sim/README.md`](./sim/README.md) for
   build instructions, [`sim-base/INTEGRATION.md`](../../sim-base/INTEGRATION.md) for
   the "adopt this in my project" walkthrough.

Across all three layers, the discovery surface is a single command:

```bash
esp-harness manifest --json
# returns toolkit_commands + device.commands (?help json) + device.scenes (scene list)
```

If a capability isn't in `manifest`, it doesn't exist for the AI.

## Scene roster (18)

| # | Roman | Name | Peripheral | sim? |
|---|---|---|---|---|
| 0 | I | Halo | display | ✓ |
| 1 | II | Grid | display | ✓ |
| 2 | III | Bloom | display | ✓ |
| 3 | IV | Tilt | IMU accel | ✓ mouse-driven |
| 4 | V | Pulse | PMIC | ✓ static state |
| 5 | VI | Echo | speaker | — async audio |
| 6 | VII | Vault | SD card | — fatfs |
| 7 | VIII | Whisper | BLE | — radio |
| 8 | IX | Spectrum | WiFi | — radio |
| 9 | X | Cell | PMIC | ✓ |
| 10 | XI | Keys | buttons | ✓ SDL 1/2 → BOOT/USER |
| 11 | XII | Listen | mic + loopback | — DMA |
| 12 | XIII | Tone | speaker preset | ✓ no-op |
| 13 | XIV | System | SoC introspection | ✓ static |
| 14 | XV | Glow | AMOLED brightness | ✓ logged |
| 15 | XVI | Spin | IMU gyro | ✓ mouse-driven |
| 16 | XVII | Survey | WiFi (data-table) | — task lifecycle |
| 17 | XVIII | Sniff | BLE (data-table) | — task lifecycle |

Full mapping in [`docs/scenes-map.md`](./docs/scenes-map.md).

## Hardware

* **Board:** Waveshare ESP32-S3-Touch-AMOLED-2.16
* **Display:** 2.16″ round AMOLED, 466 × 466, **CO5300** controller, QSPI
* **Touch:** **CST9217** (not CST816 as some product pages say), I²C on GPIO 14/15
* **Compute:** ESP32-S3 dual-core LX7, 8 MB OPI PSRAM, 16 MB QIO Flash
* **IMU:** QMI8658C 6-axis, I²C @ 0x6B (shared bus with touch) — drives Tilt
* **PMIC:** AXP2101, I²C @ 0x34 (shared bus) — battery / VBUS / charge state
* **Audio:** ES8311 codec, duplex I²S on GPIO 8/9/10/42/45, PA on GPIO 46
* **Storage:** microSD via 4-bit SDIO on GPIO 1/2/3 (MX1.25 connector)
* **Radio:** ESP32-S3 internal 2.4 GHz — WiFi b/g/n + BLE 5.0

## Software stack

* **ESP-IDF v6.0.1** (installed via [EIM](https://docs.espressif.com/projects/idf-im-ui/))
* **LVGL 9.4.0** (via component manager)
* **esp_lcd_co5300** 2.0.3 (official Espressif)
* **waveshare/esp_lcd_touch_cst9217** *
* Waveshare BSP, vendored at `components/esp32_s3_touch_amoled_2_16/` with three
  small patches for ESP-IDF v6.0 compatibility (see top comments in those files)

## What this firmware does that's not obvious

Beyond the screen art, the firmware ships a **harness command channel** —
a line-based serial protocol an AI agent (or anyone) can drive over the
USB-Serial/JTAG console:

| command                         | reply / effect |
|---------------------------------|----------------|
| `?ping`                         | `OK: pong` |
| `?help`                         | list registered commands |
| `?stat`                         | `OK: {"fps":62,"heap_free":...,"scene_idx":0,...}` |
| `?dump [w=N]`                   | `DUMP_BEGIN w=128 h=128 fmt=RGB565LE ...` + base64 payload + `DUMP_END` |
| `?reset`                        | soft reset |
| `tap` / `tap X Y`               | synthesise a touch event (center, or at (X,Y)) |
| `swipe X1 Y1 X2 Y2 [DUR_MS]`    | synthesise a drag gesture |
| `scene next` / `scene N`        | navigate scenes |
| `?sensor`                       | IMU: `{"ready":true,"accel":[gx,gy,gz]}` in g (delta from neutral pose) |
| `?power`                        | PMIC: `{"vbus","battery","charge","percent","voltage_mv"}` |
| `audio tone FREQ DUR_MS [VOL]`  | sine wave through ES8311 speaker; replies with actual `elapsed_ms` |
| `?sd`                           | SD card mount state + capacity / free (FATFS `statvfs`) |
| `wifi scan [TIMEOUT_MS [N]]`    | STA scan; returns top-N APs by RSSI |
| `ble scan [DUR_MS [MAX_N]]`     | passive observer; returns unique devices + raw `adv_events` count |

The shapes (`OK:` / `ERR:` / `EVT:` framing, `_BEGIN_/_END_` payload markers)
are the contract the toolkit's `console_session.py` speaks against.

## Building

```powershell
# from a shell with ESP-IDF activated (or use the EIM PowerShell)
cd examples\aurora            # from the esp-harness monorepo root
idf.py set-target esp32s3
idf.py -p COM9 build flash monitor
```

…or, if you have the toolkit installed:

```powershell
esp-harness run --project examples\aurora `
                --seconds 5 --until "aurora ready" --json
esp-harness screenshot --out aurora.png --size 192
esp-harness monitor --tap            # tap the screen via console
```

## Project layout

```
.
├── CMakeLists.txt                # top-level (project(aurora))
├── partitions.csv                # custom: 8 MB factory + 7 MB spiffs
├── sdkconfig.defaults            # PSRAM 80M / QIO flash / LVGL 9 tuned / BT NimBLE EXTERNAL
├── components/
│   └── esp32_s3_touch_amoled_2_16/   # Waveshare BSP (v5.5→v6.0 patched)
├── main/
│   ├── aurora_main.c             # boot order: bsp → console → UI → harness → peripherals
│   ├── ui_shell.c/h              # roman numeral + 5 indicator dots + tiny fps overlay
│   ├── harness/
│   │   ├── console_protocol.c/h  # generic line parser, OK/ERR/EVT framing
│   │   ├── harness_commands.c/h  # 11 commands across all peripherals
│   │   └── scene_framework.c/h   # scene_t lifecycle + container hand-off
│   ├── scenes/
│   │   ├── scene_halo.c          # I.  concentric rings, ice blue
│   │   ├── scene_grid.c          # II. radial compass rose, warm amber
│   │   ├── scene_bloom.c         # III. phyllotaxis spiral, soft rose
│   │   ├── scene_tilt.c          # IV. IMU-driven spirit-level bubble
│   │   ├── scene_pulse.c         # V.  battery-state breathing ring
│   │   └── scenes.h
│   ├── peripherals/
│   │   ├── imu.c/h               # QMI8658C accel (mg → g, neutral-pose calibration)
│   │   ├── pmic.c/h              # AXP2101 STATUS1/2 + VBAT + SoC
│   │   ├── audio.c/h             # ES8311 speaker; sine-tone generator
│   │   ├── sdcard.c/h            # bsp_sdcard_mount + statvfs
│   │   ├── wifi.c/h              # STA scan (lazy init at first scan)
│   │   └── ble.c/h               # NimBLE observer (eager init at boot, PSRAM pool)
│   └── idf_component.yml         # lvgl/lvgl ^9, waveshare/qmi8658 ^1.0.0
└── KNOWN_ISSUES.md               # 11 post-mortems; read before debugging similar
```

## Design notes (taste)

* **Pure black backgrounds**, full stop. On AMOLED, `#000` literally turns
  the subpixel off — anything else flares the photoreceptor adaptation and
  spoils the contrast you paid for.
* **One accent colour per scene.** No multi-hue palettes; no gradients.
  Halo is ice blue, Grid is warm amber, Bloom is soft rose. Limited
  palette reads more refined than rainbows on a small round screen.
* **Hairlines.** Border widths of 1 or 2 px. No filled chrome boxes.
* **Roman numerals** for scene indices instead of digits. Slightly
  ceremonial; reads "considered" rather than "demo".
* **Indicator dots** *below* the centre, never edges. The round bezel
  eats anything near the corners.
* No `0xRGB` neon, no gradient text, no glassmorphism, no animated
  loading spinners. If a scene needs motion it earns it via slow,
  intentional movement — not by default.

## Known issues / post-mortems

See [`KNOWN_ISSUES.md`](./KNOWN_ISSUES.md) for full diagnoses. The eleven
entries cluster into a few themes:

* **QSPI DMA underflow with PSRAM draw buffer** (§1, **RESOLVED**). The BSP
  default of `use_psram=true` on the LVGL adapter couldn't sustain
  full-screen invalidations. One-line BSP patch.
* **LVGL is not thread-safe** (§9c). Console-task scene switching that
  calls LVGL APIs without `bsp_display_lock` deadlocked once Tilt
  introduced a 20 Hz timer that mutates objects from the LVGL task.
* **QMI8658 traps** (§9a/b/d). Vendor `qmi8658_init` is a final-path init —
  re-configuring CTRL2 after it puts the chip in a saturated stuck state.
  Output unit is **milli-g**, not g.
* **NimBLE + internal SRAM** (§11, **RESOLVED**). "7 MB heap free" is a
  red herring — FreeRTOS stacks and BT pools live in internal SRAM, of
  which only ~26 KB was free after LVGL + audio. Switched
  `BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y` to route NimBLE pools to PSRAM.
* **WiFi + BLE coexistence** (§11). `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y`
  is required even for BLE-alone use on ESP32-S3.

## License

MIT (firmware code). BSP and managed components retain their original
upstream licenses.
