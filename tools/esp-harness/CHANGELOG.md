# Changelog

All notable changes to esp32-harness-toolkit. Format: keep-a-changelog,
versions tagged in git.

## [1.0.0] — 2026-05-18

First formal release. Twelve commands wrapping ESP-IDF v6.0.1 (EIM-installed)
into an agent-friendly CLI with JSON output and semantic exit codes.

### Discovery surface

* `manifest` — one-shot enumeration of toolkit commands + device commands +
  device scenes. JSON output is **the** entry point for a fresh AI session:
  if it isn't in `manifest`, the capability does not exist for the AI.
* `console` — generic transport: send any console line, get OK / ERR plus
  parsed JSON payload (`--payload TAG`).

### Commands (12)

| Command | Purpose |
|---|---|
| `port detect` | Auto-select unique ESP32 USB port (VID-aware tier classification) |
| `port list` | All candidate serial ports |
| `build` | Wrap `idf.py build` with EIM env hydration |
| `flash` | Wrap `idf.py flash` |
| `monitor` | Capture serial output with timeout + `--until` regex early-exit |
| `run` | build + flash + capture in one shot |
| `manifest` | Discovery surface aggregator |
| `console` | Generic console transport |
| `screenshot` | LVGL framebuffer dump → PNG |
| `backtrace` | Auto-decode addr2line from panic logs |
| `bench` | Standardised perf snapshot (fps, heap, audio, BLE, SD, IMU) |
| `audio` | Audio test commands (tone / loopback / volume) |

### Exit codes (see `src/esp_harness/exit_codes.py`)

* `0` OK
* `10` no_device, `11` device_busy, `12` ambiguous
* `20` build_failed, `21` project_not_found
* `30` flash_failed
* `40` monitor_timeout
* `100` env_not_configured

### Environment hydration

* PowerShell subprocess dot-sources the EIM activation script
  (`C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1`), captures
  the resulting env, caches it. Avoids the directory-layout conflicts of
  the standard `idf_tools.py export` flow.
* Windows OpenSSH path quirks documented in `AGENT.md` §12 Gotchas.

### Documentation

* `AGENT.md` — section 0 "Bootstrap" (manifest-first) + section 12 "Gotchas"
  (DTR/RTS, PA pin, codec_dev_read return semantics, WiFi/BLE coexist,
  LVGL ASCII-only, etc).
* `tools/check_manifest.py` — CI lint: every CLI command must be registered
  in `TOOLKIT_COMMANDS`.

### Companion firmware

* Verified against [esp32-harness-showcase](https://github.com/Caldis/esp32-harness-showcase)
  v1.0 — 16 scenes, 17 console commands.
