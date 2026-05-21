# Changelog

All notable changes to the Aurora showcase firmware. Format: keep-a-changelog,
versions tagged in git.

## [1.4.0] — 2026-05-20

Ten-iteration scaffold-completion sweep (H1-H10). Everything that was
genuinely generic but still in `main/` got lifted into the
`aurora-harness` component. The component lost its hard board binding.

### Component lifts (everything-now-in-aurora-harness)

* **`cmd_scene`** moved from `main/harness/harness_commands.c` to
  `default_cmds.c`. The application's UI chrome (e.g. `ui_shell`) hooks
  in via the new **`scene_fw_set_change_listener`** API instead of
  being hard-coded inside the command.
* **`scene_fw_find_by_id`** new helper, exposed from the framework.
* **`cmd_tap` + `cmd_swipe`** lifted (pure LVGL indev synthetic input).
* **`cmd_dump`** + the entire screen-snapshot pipeline (PSRAM buffer
  management, top-layer compositing, base64 emit) moved to a new
  `screenshot.c` + `harness/screenshot.h` public header.
* **`harness_default_register()`** now registers `?stat` / `scene` /
  `tap` / `swipe` / `?dump` in a single call.

### New primitives

* **`harness_progress(text, percent)`** + `harness_progress_dismiss()` —
  long-running task overlay (label + bar) on `lv_layer_top()`. Sister
  to `harness_toast` with an explicit lifecycle.
* **Scene XX Track** — reference demo: 5-second simulated download
  with BOOT-start / USER-cancel. Bumps scene count from 19 → 20.
* **`harness/bsp_iface.h`** — minimum BSP surface declared (just
  `bsp_display_lock` / `bsp_display_unlock`). Component no longer
  hard-requires `esp32_s3_touch_amoled_2_16`; any board that exposes
  those two symbols works.

### Toolkit additions (sibling repo)

* **`esp-harness doctor`** — 8-point env health check: ESP-IDF / cmake /
  Pillow / pyserial / MinGW gcc / SDL2 / serial port / sibling repo.
  Per-check status + install hint.
* **`esp-harness test`** — runs `pytest tools/tests/`. Three integration
  tests: doctor health, manifest completeness, sim diff regression.
* `console --repl` slash commands (already in v1.3), preserved.

### Docs

* **`components/aurora-harness/PORTING.md`** — multi-board adoption
  guide: minimum BSP shim, integration sequence, 8-step checklist.
* **`components/aurora-harness/RELEASING.md`** — version-bump rules
  (semver), 4-step release process, `compote upload` for future
  registry publication, backwards-compat policy.
* `idf_component.yml` — version bumped to 1.4.0, `targets: esp32s3`,
  `tags`, multi-line description; registry-ready (not yet published).
* `docs/harness-report.html` v3 — adds Phase H timeline + new API
  surface entries; roadmap reflects "first tier cleared, scaffold
  surgery complete".

## [1.3.0] — 2026-05-19

Ten-iteration sprint (G1-G10) hardening the scaffold framing.

### Added

* **`harness_toast(text, ms)`** in aurora-harness — fire-and-forget
  overlay notification on `lv_layer_top()`, thread-safe via
  `lv_async_call`. New `harness/toast.h`.
* **`harness_default_register()` + `?stat`** moved into the component
  as the first "opt-in default command". Consumers call it once after
  `console_protocol_init()` to gain `?stat` (fps / heap / scene / uptime).
* **Scene XIX Notify** — reference implementation of `harness_toast()`
  usage. BOOT cycles 5 toast variants, USER long-holds.
* **`scene_t.description` + `scene_t.tags`** optional metadata fields.
  Surfaced in `scene list` JSON so AI agents reading the manifest see
  per-scene purpose without grepping source. 7 scenes carry initial
  descriptions; the rest opt-in incrementally.
* **`AGENT.md`** — onboarding manual for the next AI session: three-layer
  architecture, file-placement decision table, standard workflows,
  known-gotcha cheatsheet, cardinal rule (don't break the manifest).
* **`docs/harness-report.html`** — v2 visual progress report (editorial
  style, scaffold-mindset reframing).

### CI

* **GitHub Actions workflow `.github/workflows/sim-diff.yml`** — ubuntu
  runner, apt-installed SDL2, clones LVGL 9.4 to /tmp, builds sim,
  runs `esp-harness sim diff` against `sim/golden/`. PRs blocked on
  UI regression.
* `sim/CMakeLists.txt` cross-platform fix: the `SDL2/` subdir include
  is now `EXISTS`-guarded so Linux's `find_package(SDL2)` (which sets
  SDL2_INCLUDE_DIRS to `/usr/include/SDL2` directly) doesn't get a
  bogus nested path.

## [1.2.0] — 2026-05-19

### Promoted to scaffold

Aurora is no longer just a one-off demo — it's now a **reference firmware**
for a reusable AI-driven dev-loop scaffold.

### Added

* **`components/aurora-harness/`** — reusable ESP-IDF component. Lifted
  `console_protocol` + `scene_framework` out of `main/harness/` into a
  proper component with its own `CMakeLists.txt`, `idf_component.yml`,
  and a 350-line `README.md` documenting the protocol contract, scene
  lifecycle, threading model, integration recipe, and known limits.
  Drop into any ESP-IDF + LVGL project via `EXTRA_COMPONENT_DIRS`.
* **`sim/INTEGRATION.md`** — adopt-the-sim-template guide. Per-file role
  (generic vs project-specific), mock pattern (static / SDL-driven /
  async-task), ESP-IDF stub philosophy, BMP-not-PNG rationale, gotchas
  table.
* **`sim/` 11 scenes wired** (was 3): Halo / Grid / Bloom / Tilt / Pulse
  / Cell / Keys / Tone / System / Glow / Spin. Mock IMU follows SDL
  mouse, mock keys take SDL key 1/2 as BOOT/USER.
* **`esp-harness sim diff` + `update-golden`** in toolkit. Per-scene
  thresholds via `SCENE_TOLERANCES` (Pulse 5% for the breathing
  animation, others 1%). Pillow-based pixel diff via histogram of
  per-pixel-max-channel-delta.
* **`sim/golden/`** committed: 11 reference BMPs (one per host-runnable
  scene) plus README explaining when to refresh vs not.
* **`scene <id>` lookup**: `scene halo` / `scene survey` / etc. work
  alongside numeric `scene 3`. Unknown ids return a helpful ERR with
  the available options.

### Changed

* `main/harness/{console_protocol,scene_framework}.{c,h}` moved into the
  new component. All 20 callers updated to use `#include
  "harness/scene_framework.h"` (namespaced).
* `main/CMakeLists.txt` consumes `aurora-harness` via REQUIRES.
* `sim/CMakeLists.txt` consumes scene_framework source directly from
  the new component path (no IDF component manager on host).

### Fixed

* **`bsp_display_lock(0)` deadlock** in `cmd_scene`: non-blocking try-lock
  fails silently on busy system, console task hangs in subsequent
  `lv_obj_*` calls → task watchdog at +5s. Switched to `bsp_display_lock(-1)`
  (wait forever) — user-initiated commands can afford to wait. Exposed
  by 18-scene rendering pressure but the bug was pre-existing.
* **MAX_SCENES silent drop**: registering scene 17 past the old `16` cap
  silently dropped it. Raised to 24 + `ESP_LOGE` on overflow.

## [1.1.0] — 2026-05-18

### Added

* **Scene XVII Survey** — WiFi APs as a data table (SSID + channel + RSSI +
  auth + bar). Data-table complement to IX Spectrum. BOOT=rescan,
  USER=toggle sort.
* **Scene XVIII Sniff** — BLE devices as a data table (name/addr + RSSI +
  type + bar). Data-table complement to VIII Whisper. BOOT=rescan,
  USER=toggle name/addr display.
* **`?ota` console cmd** — partition state + rollback control:
  `?ota info` (running/boot/next slots + state + rollback flag),
  `?ota mark-valid`, `?ota rollback`. Real OTA download requires a
  partition table with `ota_data` + WiFi credentials (Phase 2).
* **Discovery-surface guardrail** — `scene_fw_register` now logs an error
  when overflowing `MAX_SCENES` (was silent). Cap raised 16 → 24.

### Fixed

* **Boot flicker** — `settings_init` + `last_scene` restore moved before
  LVGL unlock, so the auto-shown scene 0 from the first
  `scene_fw_register` never paints when the device booted on a different
  scene.

## [1.0.0] — 2026-05-18

First formal release. Sixteen scenes, every onboard peripheral exercised, full
discovery surface for AI-driven testing.

### Scenes (16)

| # | Roman | Name | Peripheral |
|---|---|---|---|
| 0 | I | Halo | display (pure render) |
| 1 | II | Grid | display (pure render) |
| 2 | III | Bloom | display (pure render) |
| 3 | IV | Tilt | IMU accel (QMI8658) |
| 4 | V | Pulse | PMIC (AXP2101) |
| 5 | VI | Echo | speaker (ES8311) |
| 6 | VII | Vault | SD card (hot-plug + bench + format) |
| 7 | VIII | Whisper | BLE (NimBLE observer) |
| 8 | IX | Spectrum | WiFi (STA scan) |
| 9 | X | Cell | PMIC dashboard |
| 10 | XI | Keys | BOOT / USER / PWR buttons |
| 11 | XII | Listen | mic + speaker loopback (ES7210 + ES8311) |
| 12 | XIII | Tone | speaker preset sweep |
| 13 | XIV | System | SoC + heap + thermals (3 sensors) |
| 14 | XV | Glow | AMOLED brightness control |
| 15 | XVI | Spin | IMU gyro live |

### Console protocol

* Line-based OK / ERR / EVT framing + multi-line payload tags (HELP, SCENES,
  DUMP, ...).
* Discovery surface: `?help json` emits HELP_BEGIN/END JSON manifest of every
  registered command; `scene list` emits SCENES_BEGIN/END.
* 17 commands: `?ping`, `?reset`, `?help`, `?stat`, `?dump`, `?sys`, `?sensor`,
  `?power`, `?audio`, `?sd`, `?wifi`, `?ble`, `?keys`, `scene`, `tap`, `swipe`,
  `audio`.

### Persistence (NVS)

* Settings layer with cache + 5 s commit throttle (`peripherals/settings.{c,h}`).
* Restored on boot **before** LVGL render: volume, brightness, last scene —
  no boot flicker.

### Build / tooling

* ESP-IDF v6.0.1 (via EIM), LVGL 9.4.
* Driven end-to-end by [esp32-harness-toolkit](https://github.com/Caldis/esp32-harness-toolkit)
  v1.0 — see toolkit README for the workflow.
* Discovery: `esp-harness manifest --json` enumerates every device command +
  scene + toolkit command in one shot.
