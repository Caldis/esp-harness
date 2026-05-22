# esp-harness — AI Agent Manual

> **You (the AI) are the primary user of this toolkit.** Humans run it occasionally for sanity checks; you run it every iteration.

---

## 0. Bootstrap (do these two things first, every session)

```bash
# 1. Enumerate everything the toolkit + device can do (one JSON document):
esp-harness manifest --json

# 2. Skim section 12 (Gotchas) below for the prose-only knowledge that
#    can't be expressed as a command schema.
```

**Convention:** if a capability is not listed in `manifest`'s output, **it doesn't exist for you**. Do not grep firmware source looking for it — if you find something useful in source, register it (add it to the firmware's `?help` output and/or `TOOLKIT_COMMANDS` in `commands/manifest.py`) so the next session also sees it.

`manifest` covers three layers:

| Layer | Where the data comes from |
|---|---|
| `toolkit_commands` | Static dict in `commands/manifest.py::TOOLKIT_COMMANDS` |
| `device.commands` | Live `?help json` round-trip to the connected board |
| `device.scenes` | Live `scene list` round-trip |

**Adding a new toolkit subcommand:** append an entry to `TOOLKIT_COMMANDS`. CI lint (`tools/check_manifest.py`) fails the build if a registered subcommand is missing from the list.

**Adding a new firmware console command:** call `console_protocol_register(...)`. `?help json` will automatically surface it. No further wiring needed.

**Adding a new scene:** call `scene_fw_register(...)`. `scene list` will automatically surface it.

---

## 1. What it is

A CLI that wraps ESP-IDF tooling (`idf.py`, esptool, serial monitor) with **structured JSON output** and **semantic exit codes**, so an agent can drive a full edit → build → flash → observe → decide loop without parsing free-form text or hanging on interactive monitors.

**Path:** `D:\Code\esp32-harness-toolkit\`
**Entry point:** `esp-harness` (added to user's PowerShell profile by `install.ps1`)
**Source of truth:** `src/esp_harness/`

## 2. The mental model

```
   ┌─────────────────┐
   │  YOU (the AI)   │
   └────────┬────────┘
            │ subprocess + JSON
            ▼
   ┌─────────────────────────────────────────────────┐
   │  esp-harness CLI  (this toolkit)                │
   │   port detect | build | flash | monitor | run   │
   └────────┬────────────────────────────────────────┘
            │ activates EIM env per call (cached)
            ▼
   ┌─────────────────────────────────────────────────┐
   │  ESP-IDF v6.0.1  (installed via EIM)            │
   │   idf.py · esptool · cmake · gcc · pyserial     │
   └─────────────────────────────────────────────────┘
            ▼
        target board (Waveshare ESP32-S3-Touch-AMOLED-2.16)
```

You **always call via `--json`**. Human text mode is for humans peering over the shoulder.

## 3. Universal flags

| Flag | Meaning |
|---|---|
| `--json` | Emit one JSON object on stdout. **Always pass this when you call it.** |
| `--verbose` / `-v` | Extra diagnostic lines on stderr (does not affect stdout JSON). |
| `--project PATH` | The ESP-IDF project (where `CMakeLists.txt` lives). Default: cwd. |
| `--port COM_N` | Serial port (e.g. `COM9`). Default: auto-detect, fail if ambiguous. |
| `--quiet` | Suppress live stderr output from idf.py (build/flash subcommands). |

## 4. Exit codes (the contract)

Read `src/esp_harness/exit_codes.py` for the authoritative list. Summary:

| Code | Meaning | Your reaction |
|---:|---|---|
| 0 | success | proceed |
| 1 | generic / unhandled exception | investigate; this is a bug in the toolkit |
| 2 | CLI misuse (bad args) | fix your invocation |
| 10 | no ESP32 port found | ask user to plug in board |
| 11 | port busy (open in another app) | tell user to close VS Code Serial Monitor / PuTTY |
| 12 | multiple ESP32 candidates | call again with explicit `--port` |
| 20 | build failed | read `details.errors[]` in JSON, fix source |
| 21 | project not found (no CMakeLists.txt) | check `--project` path |
| 30 | flash failed | check device state, retry once |
| 40 | monitor timed out (with `--require-match`) | pattern never appeared — bug in firmware? |
| 100 | EIM environment not found | toolkit re-install needed |

**Branch on exit code, not on parsed stdout.**

## 5. Command reference

### 5.1 `port list [--esp-only]`

Show all serial ports, optionally filtered to ESP32 candidates.

```json
// --json output
{
  "ok": true,
  "count": 1,
  "ports": [
    {
      "port": "COM9",
      "description": "USB Serial Device (COM9)",
      "vid": "0x303A", "pid": "0x1001",
      "serial_number": "A4:CB:8F:D7:56:F4",
      "manufacturer": "Microsoft",
      "product": "USB Serial Device",
      "location": null,
      "hwid": "USB VID:PID=303A:1001 SER=A4:CB:8F:D7:56:F4",
      "tier": "A",
      "chip_guess": "USB-Serial/JTAG"
    }
  ]
}
```

Tier `A` = Espressif native (VID `0x303A`). Tier `B` = USB-UART bridge IC (CH340, CP210x, FTDI, PL2303). Anything else is rejected before getting into the list under `--esp-only`.

### 5.2 `port detect`

Pick the single best ESP32 port.

```json
// success — exactly one candidate
{ "ok": true, "port": "COM9", "tier": "A", "chip_guess": "USB-Serial/JTAG",
  "vid": "0x303A", "pid": "0x1001", "serial_number": "...",
  "candidates_considered": 1 }

// exit 10 — no candidates
{ "ok": false, "error": "No ESP32-like serial port found.", "exit_code": 10,
  "candidates": [] }

// exit 12 — ambiguous
{ "ok": false, "error": "Multiple ESP32 candidates (2). Pass --port explicitly.",
  "exit_code": 12,
  "candidates": [ {...}, {...} ] }
```

**Use this before flash/monitor** if you don't have a `--port` cached from earlier in the session.

### 5.3 `build`

Run `idf.py build` and parse the result.

```bash
esp-harness build --project D:\Code\ESP32-DEMO\blink --json --quiet
```

```json
// success
{
  "ok": true,
  "elapsed_ms": 17361,
  "project": "D:\\Code\\ESP32-DEMO\\blink",
  "warnings": [
    { "file": "...", "line": 42, "col": 8, "level": "warning",
      "message": "unused variable 'foo'", "raw": "..." }
  ],
  "artifacts": {
    "elf": "D:\\Code\\ESP32-DEMO\\blink\\build\\blink.elf",
    "bin": "D:\\Code\\ESP32-DEMO\\blink\\build\\blink.bin",
    "map": "D:\\Code\\ESP32-DEMO\\blink\\build\\blink.map"
  },
  "n_warnings": 0
}

// failure (exit 20)
{
  "ok": false, "error": "build failed (exit 1) after 5.3s", "exit_code": 20,
  "elapsed_ms": 5340, "returncode": 1, "n_errors": 1, "n_warnings": 0,
  "errors": [
    { "file": "main/blink.c", "line": 27, "col": 5, "level": "error",
      "message": "'gpio_setlevel' undeclared (did you mean 'gpio_set_level'?)",
      "raw": "main/blink.c:27:5: error: ..." }
  ],
  "warnings": []
}
```

Error parser covers: gcc/clang `file:line:col: error/warning:`, `ninja: error:` / `FAILED:`, `ld.lld: error:`. If the regex misses, the error string still appears in `details.tail` (last 30 lines). When in doubt, re-run without `--quiet` to see raw output.

### 5.4 `flash`

```bash
esp-harness flash --project <proj> [--port COM9] [--baud 460800] --json
```

Auto-detects port if omitted. Returns:

```json
// success
{ "ok": true, "elapsed_ms": 9871, "port": "COM9", "baud": 460800,
  "wrote_bytes": 168448, "verified": true }

// exit 11 — port busy
{ "ok": false, "error": "Port COM9 busy or access denied.", "exit_code": 11,
  "port": "COM9", "elapsed_ms": 1230, "returncode": 2 }

// exit 30 — flash failed (other)
{ "ok": false, "error": "flash failed (exit 2) after 2.5s", "exit_code": 30,
  "port": "COM9", "baud": 460800, "elapsed_ms": 2530, "returncode": 2,
  "tail": [ "...", "...", "..." ] }
```

### 5.4b `tap` — the AI's "finger"

Sends a single byte to the device over serial. Pairs with a firmware-side
listener that synthesises a touch event whenever any byte arrives. See
`D:\Code\ESP32-DEMO\image-viewer\main\image_viewer_main.c` for the canonical
firmware pattern (`serial_tap_task` + `inject_tap_async`).

```bash
esp-harness tap --port COM9 --count 3 --interval-ms 500 --json
```

```json
{ "ok": true, "port": "COM9", "baud": 115200, "count": 3,
  "interval_ms": 500, "byte": "\n", "elapsed_ms": 1004 }
```

**However**, Windows COM ports are exclusive: you cannot run `tap` and
`monitor` against the same port simultaneously from two processes. For the
common AI workflow ("inject N taps and capture what the device says"), use
`monitor --tap --tap-count N` — it does both in one open/close cycle. See
§5.5 below.

### 5.4c `screenshot` — the AI's REAL eyes

Pull a PNG of the device's framebuffer over the console. Needs firmware
support: a `?dump` command that emits base64-encoded RGB565. See
[Aurora's `harness_commands.c`](https://github.com/Caldis/esp-harness/blob/master/examples/aurora/main/harness/harness_commands.c)
for the canonical consumer; the implementation lives in
[`aurora-harness/src/screenshot.c`](https://github.com/Caldis/esp-harness/blob/master/components/aurora-harness/src/screenshot.c).

```bash
esp-harness screenshot --out check.png --size 128 --json
```

```json
{ "ok": true, "port": "COM9", "out": "check.png",
  "w": 128, "h": 128, "bytes": 32768, "elapsed_ms": 4109 }
```

**Size recommendations:**

| `--size` | Bytes on wire | Wall time | Reliability | Use when |
|---------:|---------------|-----------|-------------|----------|
| 96       | ~18 KB        | ~2.4 s    | 100 %       | quick visual probe inside a tight loop |
| **128**  | **~32 KB**    | **~4 s**  | **100 %**   | **daily default** |
| 192      | ~73 KB        | ~9 s      | ~95 %       | "look closely" — careful around heartbeat boundaries |
| 256+     | ≥130 KB       | n/a       | reliably fails | not implemented; needs binary protocol w/ CRC |

The default `--size 128` is the sweet spot because (a) it's below the
USB-CDC long-burst danger zone, and (b) at 4 s it finishes between
typical ESP_LOG heartbeats (10 s).

**Behind the scenes** (it took 4 layers of fix to get here — the
historical post-mortem is preserved in the archived
[`esp32-harness-showcase` KNOWN_ISSUES § 5](https://github.com/Caldis/esp32-harness-showcase/blob/master/KNOWN_ISSUES.md#5-dump--the-four-layer-screenshot-pipeline-post-mortem)):
* firmware box-filter average (no rainbow speckle on AA edges)
* host-side per-line base64 validation (drop interleaved ESP_LOG)
* firmware composites `lv_layer_top` over the screen snapshot
* host timeout auto-scales with size

If you're writing a NEW project that wants `?dump`, your firmware needs:
1. A console command `?dump [w=N]`
2. Snapshot the screen + top layer, composite them
3. Box-filter downsample to N × N
4. Emit `OK: dump start <meta>\n` then `DUMP_BEGIN <meta>\n<base64 lines>\nDUMP_END\n`

See `aurora_do_snapshot` for the lv_async_call pattern that runs the
snapshot inside the LVGL task (where the mutex is already held — don't
try to `bsp_display_lock` from the console task, you'll starve under
animation load).

### 5.5 `monitor` — the AI's "eyes"

**Non-interactive**. Open port, read for N seconds (or until regex), close, return.

```bash
esp-harness monitor --port COM9 --seconds 10 --until "main_task: Calling app_main" --json
```

```json
{
  "ok": true,
  "port": "COM9", "baud": 115200, "elapsed_ms": 1247,
  "matched": "main_task: Calling app_main",
  "timed_out": false,
  "n_lines": 27,
  "n_bytes": 1689,
  "text": "...\nI (268) main_task: Calling app_main()\n...",
  "lines": [
    "ESP-ROM:esp32s3-20210327",
    "Build:Mar 27 2021",
    "...",
    "I (268) main_task: Calling app_main()"
  ]
}
```

Add `--require-match` to make missing pattern an exit 40 failure (default: returning without a match is still exit 0).

**Tap injection (AI's finger)** — `monitor --tap [--tap-count N] [--tap-interval-ms M] [--tap-delay-ms D]` writes one newline byte to the port `N` times, spaced by `M` ms, with an initial `D`-ms delay after opening. If the firmware ships a serial-tap listener (see §5.4b), each newline synthesises a touch event. Example — drive 3 taps and confirm each landed:

```bash
esp-harness monitor --port COM9 --seconds 5 --tap --tap-count 3 \
    --tap-interval-ms 700 --until "touch -> showing image 0" --json
```

> **Important**: `monitor` does **not** reset the device. We explicitly hold DTR and RTS deasserted on open (`no_reset=True` in `serial_io.capture`) so the ESP32-S3 native USB-Serial/JTAG doesn't auto-reboot when we attach. If you need fresh boot output, run `flash` (which resets after writing) immediately before — or just use `run` which sequences them correctly.

### 5.6 `run` — the AI iteration command

`build` + `flash` + `monitor`, with consistent JSON for the whole thing.

```bash
esp-harness run --project <proj> --seconds 8 --until "Turning the LED" --json --quiet
```

```json
{
  "ok": true,
  "project": "D:\\Code\\ESP32-DEMO\\blink",
  "total_elapsed_ms": 11488,
  "phases": {
    "build":   { "ok": true, "elapsed_ms": 1463, "n_errors": 0, "n_warnings": 0,
                 "artifacts": {...} },
    "flash":   { "ok": true, "port": "COM9", "baud": 460800, "elapsed_ms": 9871,
                 "returncode": 0 },
    "monitor": { "ok": true, "port": "COM9", "baud": 115200, "elapsed_ms": 147,
                 "matched": "Turning the LED", "timed_out": false,
                 "n_lines": 13, "text": "...", "lines": [...] }
  }
}
```

If any phase fails, subsequent phases are skipped, exit code reflects the failed phase, and the JSON still reports what ran. Skip the build phase with `--no-build` (use existing artifacts).

## 6. Standard workflows

### 6.1 First-time check on a new machine

```bash
esp-harness --version              # toolkit installed?
esp-harness port detect --json      # board reachable?
```

If both exit 0, you're ready.

### 6.2 Code → verify loop

```bash
# 1. Edit source files in the project (use Edit/Write tools)
# 2. Run a single iteration
esp-harness run --project D:\Code\ESP32-DEMO\blink \
                --seconds 8 \
                --until "your-expected-log-line" \
                --json
# 3. Branch on exit code + JSON content
```

Pattern your `--until` regex on a **distinctive line your new code prints**.
That way "monitor exited at 2s with a match" means "your code reached that
point", which is much faster signal than "captured 8s of logs, hope something's
in there".

### 6.3 Debug a build failure

```bash
esp-harness build --project <p> --json --verbose 2>&1 | tee build.log
# JSON on stdout, raw idf.py output on stderr (preserved by --verbose)
```

Read `details.errors[]` for parsed errors with file/line. Read `build.log` for full context if the parser missed something.

### 6.4 Crash investigation

Use the `backtrace` command — it monitors for `Backtrace:` lines and
resolves them via addr2line automatically:

```bash
# Live capture: monitor for 30 s, decode any Backtrace line that comes through
esp-harness backtrace --monitor 30 --json

# Offline: pipe an existing log into stdin
type crash.log | esp-harness backtrace --stdin --json
```

If you only want raw text without resolution, the older `monitor` flow
still works:

```bash
esp-harness monitor --seconds 30 --json > crash.json
xtensa-esp-elf-addr2line -pfiaC -e build/<proj>.elf <addresses>
```

### 6.5 Performance regression check

`bench` captures a fixed set of measurements (fps, heap, audio loopback,
BLE adv rate, SD read/write, IMU noise). Lock a baseline after a known-good
firmware, then diff every subsequent run against it.

```bash
# After verifying a build is healthy, lock its numbers as the package baseline.
# Writes to src/esp_harness/data/baseline.json (checked into git).
esp-harness bench --baseline --json

# CI / smoke check — exit 1 if any metric regresses beyond its threshold.
esp-harness bench --compare --json
```

Regression thresholds (`commands/bench.py::REGRESSION_CHECKS`):

| Metric | Direction | Threshold |
|---|---|---|
| `stat.fps` | down | 10% |
| `stat.heap_free` | down | 5% |
| `stat.int_free` | down | 10% |
| `stat.psram_free` | down | 5% |
| `audio_loopback.elapsed_ms` | up | 20% |
| `ble_scan.adv_events` | down | 40% (BLE is intrinsically noisy) |
| `sd_bench.write_kbps` | down | 20% |
| `sd_bench.read_kbps` | down | 20% |

Thresholds are **directional**: heap dropping triggers a regression, heap
growing does not. Without this asymmetry, normal variance produces
false positives. Tune `REGRESSION_CHECKS` per project; the defaults are
calibrated for the Aurora showcase on the Waveshare 2.16″ board.

The compare output's `device.elf_sha` lets you tell at a glance whether a
regression really compared apples-to-apples or you forgot to flash.

## 7. What's NOT here yet (Phase 2 backlog)

Don't try to do these with `esp-harness` yet — they're roadmapped:

- **LVGL desktop simulator** — fast UI iteration without flashing. Built at `examples/aurora/sim/` in the monorepo. Build is gated on installing SDL2 + a host C compiler — system-wide changes the user has to authorise. See `examples/aurora/sim/README.md` for the install commands. `esp-harness sim snapshot --scene N --out path.bmp` drives it.
- **Webcam capture of the physical screen** — for final visual verification.
- **Framebuffer dump over serial** — alternative to webcam; ~430 KB / capture.
- **Touch event injection** — via a debug command in firmware (`tap X Y` over UART). (`tap` console cmd exists; toolkit wrapper still TODO.)
- **`esp-harness init` project scaffold** — template generator (firmware + sim + ui shared dir).
- **`pytest-embedded`** — on-target unit/integration test harness.

If you need any of these, **flag it in `phase2-needed`** to the human and consider implementing it (the toolkit is designed to be extended — add a new module under `src/esp_harness/commands/`, register it in `cli.py`, follow the dual-mode output pattern).

## 8. Extending the toolkit

Every command is a module under `src/esp_harness/commands/`. Each module exposes two functions:

```python
def add_subparser(sub, add_common_flags) -> None: ...
def run(args, output) -> int: ...   # returns exit code
```

Register in `cli.py` (`from esp_harness.commands import X as cmd_X`, dispatch by name in `main()`).

Conventions:
- All structured output via `output.success(payload)` / `output.failure(...)`.
- Always populate `details` on failure with enough info for the AI to recover.
- Use constants from `exit_codes.py`; don't hardcode integers.
- Errors that come from the user's project (build failure, port busy) → use semantic codes (20s, 10s).
- Errors that come from the toolkit's own bugs (regex didn't compile, JSON didn't serialize) → exit code 1 + traceback in `details` under `--verbose`.

## 9. Re-install / upgrade

```powershell
cd D:\Code\esp32-harness-toolkit
.\install.ps1
```

Idempotent. Reuses `.venv`, refreshes editable install, rewrites profile shim.

If EIM upgrades ESP-IDF version, the path inside `src/esp_harness/core/idf_runner.py` may need updating (it auto-discovers `Microsoft.v*.PowerShell_profile.ps1` under `C:\Espressif\tools\` but pins `v6.0.1` as default).

## 10. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `esp-harness: command not found` | New shell hasn't loaded profile yet | Open a new PowerShell window |
| All commands exit 100 | EIM not installed or path changed | Re-run EIM; verify `C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1` exists |
| `port detect` exit 10 | Board not plugged, bad cable, or driver missing | Replug, swap to known-data USB cable, check Device Manager for "Unknown USB Device" |
| `flash` exit 11 (port busy) | Another program holds the port | Close VS Code Serial Monitor / PuTTY / any open `monitor` |
| `flash` exit 30 with "Failed to connect" | Board not in download mode / chip stalled | Hold BOOT, tap RESET, release BOOT, retry |
| `monitor` returns empty | Wrong baud (default 115200) or device silent | Check `--baud`, verify firmware is actually printing |
| Build runs but `Detected size larger than image header` warning | sdkconfig has wrong Flash size | Run `idf.py menuconfig` → Serial flasher config → Flash size = 16 MB |

## 11. One-page cheat sheet

```bash
esp-harness port detect --json                                # who am I talking to?
esp-harness build --project P --json --quiet                  # compile
esp-harness flash --project P --port COM --json --quiet       # upload
esp-harness monitor --port COM --seconds N --until R --json   # listen
esp-harness run --project P --seconds N --until R --json --quiet  # all of the above

# console — the generic transport. JSON in / JSON out.
esp-harness console --cmd "?stat" --port COM --json
esp-harness console --cmd "wifi connect ssid=\"My AP\" pass=xxx" --port COM --json

# async commands: wait for the EVT that lands AFTER the OK: ack.
esp-harness console --cmd "tap 233 233" --port COM \
                    --wait-evt "^tap_hit" --evt-timeout 2 --json
esp-harness console --cmd "?ota download url=https://..." --port COM \
                    --wait-evt "OTA progress=100" --evt-timeout 60 --json

# always check $? / exit code first; then parse JSON.
```

---

## 12. Gotchas — the irreducible prose

Things you cannot discover by reading `manifest`. Read once, internalise, don't re-learn them the hard way.

### Serial / device

* **DTR = False at port open.** The ESP32-S3 USB-Serial/JTAG peripheral treats a DTR transition as a reset signal. pyserial asserts DTR on `open()` by default, which reboots the device mid-test. Every command that opens a port goes through `ConsoleSession` or the same `dtr=False` pre-open dance. Don't replace either with naive `serial.Serial(port)`.
* **Don't run two consumers on the same port.** Only one process can have `COM9` open. If `flash` returns exit 11 ("busy"), kill any open monitor/PuTTY/VS Code Serial Monitor first.
* **`esp-harness console` is the generic transport.** Specialised CLI commands (`audio`, `screenshot`, `backtrace`, etc.) wrap it for ergonomics + hearing-safety caps. When in doubt — or for a new firmware command not yet in the manifest — fall back to `esp-harness console --cmd "..."`.

### LVGL / display

* **Never call `lv_*` APIs from a non-LVGL task without `bsp_display_lock`.** The Tilt scene's 20 Hz timer made this discoverable; cross-task races corrupt LVGL's invalidate area list and the next call wedges in `lv_inv_area`. `scene action` over the console uses `lv_async_call` to dispatch onto the LVGL task instead.
* **Scene timers must `lv_timer_pause` in `on_hide`.** Otherwise they keep mutating hidden widgets and pile invalidations until LVGL freezes.
* **LVGL built-in Montserrat is ASCII-only.** `…` `·` `±` `—` `→` render as boxes. `tools/scan_nonascii.py` lints. Stick to ASCII for label/snprintf strings (comments are fine).

### Audio

* **The PA enable pin (GPIO 46) is not driven by the BSP es8311 driver.** Sound never came out until we explicitly `gpio_set_level(BSP_POWER_AMP_IO, 1)` after every `esp_codec_dev_open(s_spk)` call. See `peripherals/audio.c::pa_force_high`. If you replace the speaker init, keep this workaround.
* **`esp_codec_dev_read` returns 0 on success, NOT a byte count.** Documented in `audio_codec_data_i2s.c:717` as `ret == 0 ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_DRV_ERR`. Counting bytes treats success as "0 bytes read" and loops forever.
* **Toolkit caps `audio tone --vol` at 30 %**. Hearing-safety: AI testing at 60–95 % was uncomfortable. The cap is host-side; the GUI BOOT/USER buttons in Scene XII can go higher.
* **Loopback playback normalises captured PCM** (peak → 28000 by default, `audio boost`) so the speaker hits useful loudness. Raw mic capture sits around −20 dBFS RMS which sounds nearly silent through this driver.

### Radios

* **WiFi and BLE share internal SRAM** and can't both be initialised on this board without help.
  * `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y` moves NimBLE pools to PSRAM (essential).
  * `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y` is required even when only BLE runs.
  * Both radios are **lazy-initialised**: BLE comes up on first `ble scan`, WiFi on first `wifi scan`. Whichever is first wins the residual internal SRAM.
  * `radio wifi` calls `esp_bt_mem_release(BTDM)` to free the BLE pool for WiFi. Going the other direction (WiFi → BLE) needs a reboot today.
* **AXP2101 PWRKEY events.** The chip only latches the negative edge (release) reliably on this die. The Keys scene's PWR pill is event-flash not level — see `scene_keys.c`.

### SD card

* **No card-detect GPIO on this board.** Hot-plug works via 2 s polling in `peripherals/sdcard.c::sdcard_poll_task` (background, low prio). Scene `on_show` enables polling, `on_hide` disables.
* **Polling task is at prio 2** so it never blocks LVGL. A failed mount can take ~300 ms, which would be a visible UI freeze if it ran on the LVGL task. It doesn't.

### IMU

* **QMI8658 `set_accel_range/odr` after `qmi8658_init` puts the chip in a saturated stuck state** (registers config'd but no fresh samples). The vendor `qmi8658_init` already configures 8 G + 1 kHz + enabled; don't poke CTRL2 afterwards.
* **Output unit is milli-g, not g.** `audio.c` / Tilt scene divide by 1000 in the getter. Easy to miss because raw values look "reasonable" either way.

### Build / flash

* **EIM PowerShell profile** activates ESP-IDF. The `core/idf_runner.py` discovers it at `C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1`.
* **PSRAM allocation pressure.** `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024` pushes small allocs to PSRAM, leaving internal SRAM for stacks and DMA. Lowering further is generally fine.

---

**End of Agent Manual.** When in doubt, run `esp-harness <cmd> --help` for argparse-generated help, or read the corresponding module under `src/esp_harness/commands/`.
