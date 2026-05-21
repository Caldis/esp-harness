# esp-harness (CLI)

[![pkg](https://img.shields.io/badge/pkg-esp--harness-b8431a)](./pyproject.toml)
[![version](https://img.shields.io/badge/version-1.5.0-1c1814)](./pyproject.toml)
[![python](https://img.shields.io/badge/python-≥3.10-1c1814)](./pyproject.toml)
[![tests](https://img.shields.io/badge/tests-3%2F3_passing-344a36)](./tests/)
[![cmds](https://img.shields.io/badge/cmds-17-1c1814)](./src/esp_harness/commands/)
[![manual](https://img.shields.io/badge/manual-AGENT.md-b8431a)](./AGENT.md)

Host-side Python CLI for the esp-harness monorepo — `build` / `flash` /
`monitor` / `run` / `sim` / `bench` / `manifest` / `doctor` / `test` /
`new` / `console` / and more. **The primary user is an AI agent.** A
human runs it occasionally for sanity checks; an LLM runs it every
iteration.

→ Part of the [esp-harness monorepo](https://github.com/Caldis/esp-harness).
For the full ecosystem (reusable C component, reference firmware, host
simulator), see the [root README](../../README.md).

> **For the full API + workflow → read [`AGENT.md`](./AGENT.md).**
> This README is a tasting menu; `AGENT.md` is the manual.

## Why this exists

ESP-IDF's stock tooling (`idf.py monitor`, etc.) is interactive — it hangs
attached to a serial port and expects a human to type. An LLM driving
firmware development needs the opposite: every command must terminate
cleanly, return structured output, and signal success/failure with a
semantic exit code.

`esp-harness` wraps `idf.py`, esptool, and pyserial with that contract.

## Capabilities

```
esp-harness port detect             # auto-find the ESP32 COM port
esp-harness port list                # list all serial ports with metadata
esp-harness build                    # idf.py build → structured JSON result
esp-harness flash                    # idf.py flash → structured result
esp-harness monitor --seconds 10     # non-interactive serial capture
esp-harness monitor --tap --tap-count 3   # capture + inject tap byte(s)
esp-harness run --seconds 8          # build + flash + monitor in one shot
esp-harness tap [--at X,Y]           # send tap byte to firmware
esp-harness screenshot --out s.png   # capture device framebuffer via ?dump
```

Each command:
* supports `--json` for one-line structured output to stdout,
* uses [semantic exit codes](./src/esp_harness/exit_codes.py)
  (10 = no device, 11 = busy, 20 = build failed, 30 = flash failed, 40 = monitor timeout, etc.),
* fails closed — never hangs, never blocks on user input.

## Quick install

```powershell
.\install.ps1
```

Creates a venv at `.venv/`, installs the package editable, and adds an
`esp-harness` shim to your PowerShell profile. Open a new window:

```powershell
esp-harness --version              # 0.1.0
esp-harness port detect            # COM9
esp-harness build --project D:\Code\esp32-harness-showcase --json
```

## Architecture

```
src/esp_harness/
├── cli.py                    # main dispatcher (argparse subcommands)
├── exit_codes.py             # 0/10/11/12/20/21/30/40/100 — the contract
├── output.py                 # human ↔ --json dual-mode emitter
├── core/
│   ├── idf_runner.py         # invokes idf.py inside EIM venv (cached env)
│   ├── ports.py              # Tier-A (VID 0x303A) + Tier-B bridge IC detection
│   ├── serial_io.py          # non-interactive capture w/ --until regex
│   │                         #  + tap injection, DTR no-reset
│   └── console_session.py    # line-based OK:/ERR:/EVT: protocol helper
└── commands/
    ├── port.py               # list / detect
    ├── build.py
    ├── flash.py
    ├── monitor.py
    ├── tap.py
    ├── run.py                # composite: build + flash + monitor
    └── screenshot.py         # ?dump → base64 RGB565 → PNG
```

The firmware-side counterpart (the device must implement `?dump`, `tap`,
`?stat` over a console line protocol for tap injection and screenshot
to work) lives in the companion repo
[`esp32-harness-showcase/main/harness/`](https://github.com/Caldis/esp32-harness-showcase/tree/master/main/harness).

## The AI loop, summarised

```
[ AI edits source ]
       ↓
esp-harness run --project P --until "ready" --json
       ↓ (build + flash + capture boot log)
[ AI parses JSON, decides ]
       ↓
esp-harness screenshot --out s.png            # "what's on the screen?"
esp-harness monitor --tap --tap-count 3        # "press the button 3×"
esp-harness monitor --port COM --seconds 10    # "what does it say now?"
       ↓
[ AI compares, iterates, commits ]
```

## Status

**v0.1 — Phase 1 + 2.5 complete:**
* Build / flash / monitor / run / port / tap (with no-reset DTR + multi-tap)
* Screenshot via firmware-side `?dump` protocol (RGB565 → PNG)
* `?stat` JSON status query

**Phase 2 backlog (next sessions):**
* Backtrace auto-decode (addr2line + ELF lookup)
* Coordinate-precise `tap --at X,Y` end-to-end (firmware-side ready, toolkit needs flag)
* `swipe X1 Y1 X2 Y2` (firmware-side ready)
* LVGL desktop simulator (`harness sim` subcommand)
* `harness init <name>` project scaffold generator
* Webcam capture for final visual verification (optical, not framebuffer)

## See also

* [`AGENT.md`](./AGENT.md) — the AI manual (full JSON schemas, exit codes, workflows)
* [companion showcase: Aurora](https://github.com/Caldis/esp32-harness-showcase) — generative-art demo that stress-tests the toolkit

## License

MIT.
