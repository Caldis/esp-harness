"""`esp-harness manifest` — single-shot capability discovery.

Run this at the start of every new AI session to learn what the toolkit
and the connected device can do, without grepping source code. Returns
a JSON document covering:

  - CLI subcommands available on the toolkit side (this binary)
  - Firmware console commands registered on the device side
  - Scenes registered in the showcase firmware

Convention (enforced by AGENT.md, see toolkit/AGENT.md "Bootstrap"):

    NEW AI SESSION:
        1. esp-harness manifest --json       # learn everything
        2. read AGENT.md "Gotchas" section   # the irreducible prose

    If a capability isn't in manifest, it doesn't exist (or it's a bug
    that the new capability didn't register itself). Don't grep source.

This command itself is fully self-describing (see TOOLKIT_COMMANDS below)
so a freshly-spawned AI can recurse one level deep just by parsing our
own output.
"""

from __future__ import annotations

import argparse
import importlib
import json
import time
from dataclasses import dataclass, field

from esp_harness import __version__
from esp_harness.core import ports as ports_mod
from esp_harness.core.console_session import ConsoleSession
from esp_harness.exit_codes import (
    AMBIGUOUS_DEVICE,
    GENERIC_ERROR,
    NO_DEVICE,
    OK,
)
from esp_harness.output import Output


# ---------------------------------------------------------------------------
# Toolkit-side manifest. Each entry is a static description of one CLI
# subcommand. Kept here (single source of truth) rather than scattered as
# `MANIFEST` constants across each command module — having them centralised
# means a new session can see everything in one read.
#
# When a new subcommand is added, append it here. The CI/lint script in
# `tools/check_manifest.py` (created alongside) verifies every CLI
# subcommand has a manifest entry.
# ---------------------------------------------------------------------------

TOOLKIT_COMMANDS: list[dict[str, object]] = [
    {
        "name": "port",
        "summary": "Enumerate or pick the ESP32 serial port.",
        "args": ["[list|pick]"],
        "exit_codes": [0, 10, 12],
    },
    {
        "name": "build",
        "summary": "Run `idf.py build` via the toolkit's IDF env wrapper.",
        "args": ["--project DIR"],
        "exit_codes": [0, 20, 100],
    },
    {
        "name": "flash",
        "summary": "Flash the firmware after build.",
        "args": ["--project DIR", "[--port COM9]"],
        "exit_codes": [0, 10, 11, 30],
    },
    {
        "name": "monitor",
        "summary": "Capture serial output for N seconds.",
        "args": ["[--seconds N]", "[--until REGEX]", "[--port COM9]"],
        "exit_codes": [0, 10, 11, 40],
    },
    {
        "name": "run",
        "summary": "build + flash + monitor in one go.",
        "args": ["--project DIR", "[--seconds N]", "[--until REGEX]"],
        "exit_codes": [0, 10, 20, 30, 40],
    },
    {
        "name": "tap",
        "summary": "Synthesise a touch tap via the firmware's `tap` command.",
        "args": ["[--at X,Y]", "[--count N]"],
        "exit_codes": [0, 10, 11],
    },
    {
        "name": "screenshot",
        "summary": "Pull a downsampled framebuffer PNG via the `?dump` command.",
        "args": ["--out PATH.png", "[--size N]"],
        "exit_codes": [0, 10, 11],
    },
    {
        "name": "audio",
        "summary": "Speaker/mic/loopback subcommands. Volume is host-capped at 30%.",
        "args": ["tone FREQ [MS [--vol N]]",
                 "mic [MS]",
                 "loopback [MS]",
                 "vol [N]",
                 "diag [--force]"],
        "exit_codes": [0, 10, 11],
        "notes": "Hearing-safety cap: --vol > 30 silently clamped. Use the GUI BOOT/USER buttons in Scene XII for louder.",
    },
    {
        "name": "console",
        "summary": "Generic command channel. Send any console line, get OK/ERR + JSON.",
        "args": ["--cmd 'STRING' | --repl", "[--payload TAG]", "[--timeout SECS]", "[--raw]"],
        "exit_codes": [0, 10, 11],
        "notes": "Use --payload TAG for commands that emit TAG_BEGIN/TAG_END blocks (`?help json` → HELP, `scene list` → SCENES, `?dump` → DUMP). --repl drops into an interactive prompt with slash commands (/quit /help /clear /payload TAG /raw /timeout SECS).",
    },
    {
        "name": "manifest",
        "summary": "Print this manifest (toolkit + device + scenes) as one JSON.",
        "args": ["[--no-device]"],
        "exit_codes": [0, 10],
        "notes": "Run me first in every new AI session.",
    },
    {
        "name": "backtrace",
        "summary": "Capture panic backtrace from serial / stdin and run addr2line.",
        "args": ["--monitor SECONDS | --stdin", "[--project DIR]", "[--elf PATH]"],
        "exit_codes": [0, 10, 11],
        "notes": "Use --monitor to actively capture; --stdin to decode an already-saved log.",
    },
    {
        "name": "bench",
        "summary": "Standardised performance snapshot (fps, heap, audio loopback, BLE rate, SD speed, sensor noise).",
        "args": ["[--quick]", "[--out PATH.json]", "[--baseline]", "[--compare]"],
        "exit_codes": [0, 1, 10, 11],
        "notes": "Diff snapshots across firmware versions to spot regressions. --baseline locks current run as package baseline (data/baseline.json); --compare diffs current vs baseline and exits 1 on threshold breach. See AGENT.md §6.5.",
    },
    {
        "name": "test",
        "summary": "Run pytest tools/tests — toolkit integration test suite.",
        "args": ["[-k KEYWORD]", "[--verbose-pytest]"],
        "exit_codes": [0, 1],
        "notes": "Three integration tests today: doctor health, manifest completeness, sim diff regression. Auto-skips if prerequisites (sim binary, sibling repo) missing.",
    },
    {
        "name": "doctor",
        "summary": "Probe toolkit + build-chain dependencies and report status.",
        "args": [],
        "exit_codes": [0, 1],
        "notes": "Checks ESP-IDF / cmake / Pillow / pyserial / MinGW gcc / SDL2 / serial port / sibling showcase repo. Required vs optional distinction in the report. JSON mode for AI consumption.",
    },
    {
        "name": "new",
        "summary": "Scaffold a new ESP-IDF + LVGL project consuming aurora-harness.",
        "args": ["<name>", "[--component-source vendor|link|depend]", "[--harness-root DIR]"],
        "exit_codes": [0, 1],
        "notes": "Generates main/<name>_main.c + scene_hello.c + CMakeLists + AGENT.md + sdkconfig.defaults. Three vendoring modes: vendor (default, self-contained copy), link (sibling EXTRA_COMPONENT_DIRS), depend (idf_component.yml registry — requires publish).",
    },
    {
        "name": "init",
        "summary": "(deprecated alias for `new`) Scaffold a new ESP-IDF + LVGL project.",
        "args": ["<name>", "[--harness-dir DIR]"],
        "exit_codes": [0, 1],
        "notes": "Pre-v1.5 alias kept for muscle memory. Forwards to `new` semantics with the legacy sibling-repo default. New code should use `new` directly.",
    },
    {
        "name": "sim",
        "summary": "Drive the host LVGL simulator (snapshot / diff / update-golden).",
        "args": [
            "snapshot --scene N --out PATH.bmp [--ms MS]",
            "diff --scenes a,b,c [--golden DIR] [--threshold 0.01] [--pixel-tol 8]",
            "update-golden --scenes a,b,c [--golden DIR]",
            "record --scene N --frames F --interval MS --out DIR [--settle MS]",
            "[--bin PATH]", "[--project DIR]",
        ],
        "exit_codes": [0, 1, 21],
        "notes": "Runs aurora_sim.exe headlessly. snapshot writes one BMP. diff compares N scenes against sim/golden/, exits 1 on regression. update-golden refreshes baseline after intentional UI changes. Scene names match sim/main.c register order: " + ", ".join([
            "halo","grid","bloom","tilt","pulse","cell","keys","tone","system","glow","spin",
        ]) + ".",
    },
]


# ---------------------------------------------------------------------------
# Device-side manifest fetching
# ---------------------------------------------------------------------------

@dataclass
class DeviceManifest:
    commands: list[dict[str, object]] = field(default_factory=list)
    scenes: list[dict[str, object]] = field(default_factory=list)
    fetched_ok: bool = False
    fetch_error: str | None = None


def _fetch_device_manifest(port: str, baud: int, timeout: float) -> DeviceManifest:
    """Hit two firmware endpoints in one serial open: `?help json` (HELP
    payload tag) for commands, `scene list` (SCENES tag) for scenes."""
    dm = DeviceManifest()
    try:
        with ConsoleSession(port, baud=baud) as session:
            r1 = session.send("?help json", timeout=timeout, expect_payload="HELP")
            if r1.ok and r1.payload:
                try:
                    body = r1.payload.decode("utf-8", errors="replace").strip()
                    parsed = json.loads(body)
                    dm.commands = parsed.get("commands", [])
                except Exception as e:
                    dm.fetch_error = f"help parse: {e}"
            r2 = session.send("scene list", timeout=timeout, expect_payload="SCENES")
            if r2.ok and r2.payload:
                try:
                    body = r2.payload.decode("utf-8", errors="replace").strip()
                    parsed = json.loads(body)
                    dm.scenes = parsed.get("scenes", [])
                except Exception as e:
                    dm.fetch_error = (dm.fetch_error or "") + f" scenes parse: {e}"
            dm.fetched_ok = dm.fetch_error is None and (dm.commands or dm.scenes)
    except Exception as e:
        dm.fetch_error = str(e)
    return dm


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser(
        "manifest",
        help="Dump toolkit + device + scene capabilities as one JSON. "
        "Run first in every new AI session.",
    )
    p.add_argument("--port", default=None, help="COM port (auto-detect if omitted).")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument(
        "--timeout", type=float, default=5.0,
        help="Per-roundtrip timeout for device queries (default 5).",
    )
    p.add_argument(
        "--no-device", action="store_true",
        help="Skip device-side queries (toolkit-only manifest).",
    )
    add_common_flags(p)


def _resolve_port(requested: str | None, output: Output) -> tuple[str | None, int]:
    if requested:
        return requested, OK
    chosen, candidates = ports_mod.detect_one_esp_port()
    if chosen is not None:
        return chosen.port, OK
    if not candidates:
        return None, NO_DEVICE
    output.failure(
        exit_code=AMBIGUOUS_DEVICE,
        error=f"Ambiguous: {len(candidates)} candidates. Pass --port.",
    )
    return None, AMBIGUOUS_DEVICE


def run(args: argparse.Namespace, output: Output) -> int:
    started = time.monotonic()
    manifest: dict[str, object] = {
        "toolkit_version": __version__,
        "toolkit_commands": TOOLKIT_COMMANDS,
        "convention": {
            "bootstrap": [
                "Run `esp-harness manifest --json` at session start.",
                "Read toolkit/AGENT.md 'Gotchas' for prose-only knowledge.",
                "Anything not in this manifest does not exist for you — "
                "don't grep source code looking for it.",
            ],
        },
    }

    if not args.no_device:
        port, code = _resolve_port(args.port, output)
        if port is None and code != OK:
            # No port — return toolkit-only manifest with a note, exit 0.
            manifest["device"] = {
                "available": False,
                "error": "No ESP32 port detected; pass --port or skip --no-device.",
            }
        else:
            dm = _fetch_device_manifest(port, args.baud, args.timeout)
            manifest["device"] = {
                "available": dm.fetched_ok,
                "port": port,
                "commands": dm.commands,
                "scenes": dm.scenes,
                "fetch_error": dm.fetch_error,
            }
    else:
        manifest["device"] = {"available": False, "skipped": True}

    elapsed_ms = int((time.monotonic() - started) * 1000)
    manifest["elapsed_ms"] = elapsed_ms

    # Always emit JSON, regardless of --json flag — manifest is a machine
    # artifact. The Output wrapper still adds a human line if non-JSON
    # mode, which is harmless.
    output.success(manifest, human=(
        f"manifest: {len(TOOLKIT_COMMANDS)} toolkit cmds, "
        f"{len(manifest.get('device', {}).get('commands', []))} device cmds, "  # type: ignore[union-attr]
        f"{len(manifest.get('device', {}).get('scenes', []))} scenes "  # type: ignore[union-attr]
        f"({elapsed_ms} ms)"
    ))
    return OK
