# Aurora scenes map

Static snapshot of every registered scene in the showcase firmware, what peripheral it exercises, and the console / CLI equivalents. The **live** version of this table is available at runtime via `scene list` (returns a JSON payload):

```bash
esp-harness console --cmd "scene list" --payload SCENES --json
# or, as part of the full discovery surface:
esp-harness manifest --json
```

This file mirrors what `scene list` returns at v1.7.1. Update it when scenes are added or renamed.

## Scene → peripheral → action

| Idx | Roman | Name | Peripheral | Short tap | Long press | Console probe | Notes |
|---:|---|---|---|---|---|---|---|
| 0 | I | Halo | display | next scene | — | — | Pure-render anchor (concentric rings) |
| 1 | II | Grid | display | next scene | — | — | Pure-render anchor (radial compass) |
| 2 | III | Bloom | display | next scene | — | — | Pure-render anchor (phyllotaxis) |
| 3 | IV | Tilt | IMU (QMI8658 accel) | next | — (always live) | `?sensor` | Bubble follows gravity; values are delta from neutral pose |
| 4 | V | Pulse | PMIC (AXP2101) | next | — (always live) | `?power` | Breathing ring, colour = charge state |
| 5 | VI | Echo | speaker (ES8311) | next | play 440 Hz 600 ms tone | `audio tone 440 600` | Async tone, ripple animation, pulse counter |
| 6 | VII | Vault | SD card | next | execute selected action (remount / bench / probe / FORMAT) | `?sd`, `?sd bench N`, `?sd format ERASE` | BOOT/USER cycle action selector; FORMAT is two-step confirm |
| 7 | VIII | Whisper | BLE (NimBLE observer) | next | 2 s passive scan | `ble scan 2000` | RSSI → radial dot position; adv_events is the radio-actually-receiving proof |
| 8 | IX | Spectrum | WiFi (STA) | next | release BLE + WiFi scan | `wifi scan 600 8` | First long-press deinits BLE; subsequent BLE access requires reboot |
| 9 | X | Cell | PMIC dashboard | next | — (always live) | `?power` | Voltage + VBUS + %/min + mV/min + ETA |
| 10 | XI | Keys | BOOT / USER / PWR buttons | next | — | `?keys` | BOOT/USER are level; PWR flashes on press event (AXP2101 only latches release edge) |
| 11 | XII | Listen | mic + speaker loopback | next | hold to record (release plays back) | `audio loopback`, `audio vol`, `audio boost` | BOOT/USER adjusts playback volume (cap 100). Capture is normalised to 28000 peak by default. |
| 12 | XIII | Tone | speaker preset sweep | next | play current freq, advance to next preset | `audio tone FREQ DUR` | 5 presets: 220 / 440 / 1000 / 2000 / 4000 Hz. BOOT/USER adjusts volume. |
| 13 | XIV | System | SoC + PMIC + IMU temps | next | — | `?sys`, `?sensor` | Three-column dashboard (compute / memory / thermal) + IDENTITY block (app / IDF / MAC). 1 Hz refresh. |
| 14 | XV | Glow | AMOLED brightness (BSP) | next | — | (no probe) | BOOT/USER ±10% brightness, live dim + persisted to NVS via `settings_set_brightness`. |
| 15 | XVI | Spin | IMU gyro (QMI8658) | next | — | `?sensor` (includes gyro[3] + temp_c) | Three signed bars X/Y/Z, ±250 dps full scale, 20 Hz. |
| 16 | XVII | Survey | WiFi STA scan, tabular | next | trigger scan (releases BLE if up) | `wifi scan` | Top-8 SSIDs as a list: SSID + channel + RSSI + auth + horizontal RSSI bar. BOOT=rescan, USER=toggle sort (rssi-desc ↔ channel-asc). Data-table complement to IX Spectrum. |
| 17 | XVIII | Sniff | BLE passive scan, tabular | next | trigger 2 s scan | `ble scan` | Top-8 devices as a list: name-or-addr + RSSI + addr type (pub/rnd) + bar. BOOT=rescan, USER=toggle name/addr display. Data-table complement to VIII Whisper. Fails if WiFi has been up since boot — surfaced via state line. |
| 18 | XIX | Notify | toast overlay (no peripheral) | next | hold to fire a 3 s toast | (no probe) | BOOT cycles through 5 toast variants (1.5 s default). Reference implementation for `harness_toast()`. |
| 19 | XX | Track | progress overlay (no peripheral) | next | — | (no probe) | 5 s simulated "download" with `harness_progress(text, percent)`. Reference implementation for the progress primitive. |

## Conventions

* **Short tap** (released < 400 ms): cycles to next scene. Handled in `aurora_main.c::on_screen_short_click`.
* **Long press** (held ≥ 400 ms): fires `scene_t::on_long_press`. Defined per-scene; `NULL` = no-op.
* **Release** (finger lift after long press): fires `scene_t::on_release`. Only Listen uses it today (stop the variable-duration recording).
* **BOOT / USER** physical buttons: each scene that wants them reads `keys_get()` in its tick and detects count-edges (rising = one press). Used by Listen / Tone / Vault for action selection or volume.

## Adding a new scene

1. Drop `main/scenes/scene_NAME.c` defining `scene_t scene_NAME = { ... }`.
2. `extern` it in `main/scenes/scenes.h`.
3. Add to `main/CMakeLists.txt` SRCS.
4. `scene_fw_register(&scene_NAME)` in `aurora_main.c` (where the other scenes are registered).
5. Bump `kSceneCount` so `ui_shell` shows the right number of dots.
6. **No further documentation work needed** — `scene list` auto-surfaces it. Update this file at next commit milestone to keep the human-readable version current.
