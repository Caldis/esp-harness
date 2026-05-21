"""`esp-harness backtrace` — capture a panic and auto-decode.

Two input modes:

  esp-harness backtrace --monitor 5       # capture 5 s of serial, decode any Backtrace: line
  esp-harness backtrace --stdin           # decode any Backtrace: line(s) from stdin

The decode path reuses `core.backtrace.decode_text`, which discovers
the ELF (build/PROJECT.elf), invokes the IDF toolchain's xtensa-esp32s3-
elf-addr2line, and returns frame objects with function names + line
numbers.

Why this CLI exists: previously every time a device panicked we copied
the backtrace addresses into an ad-hoc PowerShell + addr2line call.
This consolidates that into one command + makes it available from the
manifest.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

try:
    import serial  # type: ignore[import-untyped]
except ImportError as e:
    raise ImportError("pyserial is required.") from e

from esp_harness.core import ports as ports_mod
from esp_harness.core.backtrace import decode_text, decoded_to_jsonable
from esp_harness.exit_codes import (
    AMBIGUOUS_DEVICE,
    DEVICE_BUSY,
    GENERIC_ERROR,
    NO_DEVICE,
    OK,
)
from esp_harness.output import Output


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser(
        "backtrace",
        help="Capture a panic backtrace and decode it via addr2line.",
        description=(
            "Reads serial output (or stdin) for ESP-IDF `Backtrace: 0x... 0x...` "
            "lines, then resolves each PC to function + source location using "
            "the project's ELF. Emits JSON suitable for downstream tooling."
        ),
    )
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument(
        "--monitor", type=float, metavar="SECONDS",
        help="Open the serial port and capture for this many seconds.",
    )
    src.add_argument(
        "--stdin", action="store_true",
        help="Read serial-capture text from stdin (already-saved log).",
    )
    p.add_argument("--port", default=None, help="COM port (auto-detect if omitted).")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument(
        "--project", type=Path, default=None,
        help="Project directory (used to find build/PROJECT.elf). "
        "If omitted, the harness searches likely locations.",
    )
    p.add_argument(
        "--elf", type=Path, default=None,
        help="Direct path to ELF file. Overrides --project lookup.",
    )
    add_common_flags(p)


def _resolve_port(requested: str | None, output: Output) -> tuple[str | None, int]:
    if requested:
        return requested, OK
    chosen, candidates = ports_mod.detect_one_esp_port()
    if chosen is not None:
        return chosen.port, OK
    if not candidates:
        output.failure(exit_code=NO_DEVICE, error="No ESP32 port found.")
        return None, NO_DEVICE
    output.failure(
        exit_code=AMBIGUOUS_DEVICE,
        error=f"Ambiguous: {len(candidates)} candidates. Pass --port.",
    )
    return None, AMBIGUOUS_DEVICE


def _monitor(port: str, baud: int, seconds: float) -> str:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.1
    ser.dtr = False
    ser.rts = False
    ser.open()
    try:
        ser.dtr = False
        ser.rts = False
    except Exception:
        pass
    deadline = time.monotonic() + seconds
    buf = b""
    try:
        while time.monotonic() < deadline:
            chunk = ser.read(4096)
            if chunk:
                buf += chunk
    finally:
        try:
            ser.close()
        except Exception:
            pass
    return buf.decode("utf-8", errors="replace")


def run(args: argparse.Namespace, output: Output) -> int:
    started = time.monotonic()

    # ---- source the raw text ----
    if args.monitor is not None:
        port, code = _resolve_port(args.port, output)
        if port is None:
            return code
        try:
            text = _monitor(port, args.baud, args.monitor)
        except Exception as e:
            msg = str(e)
            if any(k in msg.lower() for k in ("access", "permission", "busy")):
                output.failure(exit_code=DEVICE_BUSY,
                               error=f"Port {port} busy: {e}")
                return DEVICE_BUSY
            output.failure(exit_code=GENERIC_ERROR,
                           error=f"monitor: {e}")
            return GENERIC_ERROR
    else:
        text = sys.stdin.read()
        port = None

    if not text.strip():
        output.success({"backtraces": [], "frames": 0, "note": "empty capture"},
                       human="no input")
        return OK

    # ---- decode ----
    decoded = decode_text(text, project_dir=args.project, elf_path=args.elf)
    frames = decoded_to_jsonable(decoded)
    elapsed_ms = int((time.monotonic() - started) * 1000)

    payload = {
        "backtraces": frames,
        "frame_count": sum(len(d.get("frames", [])) for d in frames),
        "captured_chars": len(text),
        "port": port,
        "elapsed_ms": elapsed_ms,
    }
    if not frames:
        payload["note"] = "no Backtrace: line found in input"

    human = f"{len(frames)} backtrace(s), {payload['frame_count']} frame(s) ({elapsed_ms} ms)"
    output.success(payload, human=human)
    return OK
