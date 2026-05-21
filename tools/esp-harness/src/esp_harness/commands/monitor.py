"""`esp-harness monitor` — non-interactive serial capture.

Replaces interactive `idf.py monitor` for the AI loop. Open the port, read
bytes for `--seconds` (or until `--until` regex matches), close, return the
captured text. **Never hangs.**

Note: does NOT auto-reset the device. If you need to capture the very first
boot log, run `flash` (which resets after writing) immediately before, or use
`esp-harness run` which sequences them.
"""

from __future__ import annotations

import argparse
import time

from pathlib import Path

from esp_harness.core import backtrace as bt
from esp_harness.core import ports as ports_mod
from esp_harness.core import serial_io
from esp_harness.exit_codes import (
    AMBIGUOUS_DEVICE,
    DEVICE_BUSY,
    MONITOR_TIMEOUT,
    NO_DEVICE,
    OK,
)
from esp_harness.output import Output


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser("monitor", help="Capture serial output for N seconds.")
    p.add_argument("--port", default=None, help="COM port (auto-detect if omitted).")
    p.add_argument("--baud", type=int, default=115200, help="Baud rate (default 115200).")
    p.add_argument(
        "--seconds",
        type=float,
        default=10.0,
        help="Max capture duration (default: 10s).",
    )
    p.add_argument(
        "--until",
        default=None,
        help="Regex pattern; exit as soon as accumulated output matches.",
    )
    p.add_argument(
        "--quiet",
        action="store_true",
        help="Don't tee captured lines to stderr (still in JSON payload).",
    )
    p.add_argument(
        "--require-match",
        action="store_true",
        help="If --until is given and the deadline expires without a match, "
             "exit MONITOR_TIMEOUT instead of OK.",
    )
    p.add_argument(
        "--tap",
        action="store_true",
        help="Send one newline byte shortly after opening the port (fakes a "
             "touch via the firmware's serial-tap listener). Works around "
             "Windows COM-port exclusivity that prevents separate tap + "
             "monitor processes.",
    )
    p.add_argument(
        "--tap-delay-ms",
        type=int,
        default=200,
        help="How long after opening to wait before injecting --tap (default 200ms).",
    )
    p.add_argument(
        "--tap-count",
        type=int,
        default=1,
        help="How many taps to inject in the same session (default 1).",
    )
    p.add_argument(
        "--tap-interval-ms",
        type=int,
        default=600,
        help="Spacing between successive --tap injections (default 600ms).",
    )
    p.add_argument(
        "--project",
        type=Path,
        default=None,
        help="ESP-IDF project path. If supplied, any 'Backtrace:' line in "
             "the captured output is auto-decoded via addr2line against "
             "<project>/build/*.elf and emitted in 'decoded_backtrace'.",
    )
    p.add_argument(
        "--tap-at",
        default=None,
        metavar="X,Y",
        help="Coordinate-precise tap injected at --tap-delay-ms into the "
             "session. Sends 'tap X Y\\n' (firmware console command) "
             "instead of the default newline byte. Overrides --tap.",
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
        details={"candidates": [c.to_dict() for c in candidates]},
    )
    return None, AMBIGUOUS_DEVICE


def run(args: argparse.Namespace, output: Output) -> int:
    port, code = _resolve_port(args.port, output)
    if port is None:
        return code

    output.info(f"capturing {port} @ {args.baud} for up to {args.seconds}s"
                + (f" until /{args.until}/" if args.until else ""))

    live_tee = (not args.quiet) and (not output.json_mode)

    def on_line(line: str) -> None:
        if live_tee:
            print(line)

    # --tap-at takes priority over --tap; assemble the right inject_bytes.
    if args.tap_at:
        try:
            x_str, y_str = args.tap_at.split(",", 1)
            inject_bytes = f"tap {int(x_str)} {int(y_str)}\n".encode("utf-8")
        except ValueError:
            output.failure(exit_code=2, error=f"--tap-at must be 'X,Y' (got {args.tap_at!r})")
            return 2
    elif args.tap:
        inject_bytes = b"\n"
    else:
        inject_bytes = None

    started = time.monotonic()
    try:
        result = serial_io.capture(
            port=port,
            baud=args.baud,
            seconds=args.seconds,
            until=args.until,
            on_line=on_line,
            inject_bytes=inject_bytes,
            inject_delay_ms=args.tap_delay_ms,
            inject_repeat=args.tap_count,
            inject_repeat_interval_ms=args.tap_interval_ms,
        )
    except Exception as e:
        # pyserial raises SerialException on busy ports; classify generically.
        msg = str(e)
        if "access" in msg.lower() or "permission" in msg.lower() or "busy" in msg.lower():
            output.failure(
                exit_code=DEVICE_BUSY,
                error=f"Port {port} busy: {e}",
                details={"port": port},
            )
            return DEVICE_BUSY
        output.failure(exit_code=MONITOR_TIMEOUT, error=str(e), details={"port": port})
        return MONITOR_TIMEOUT

    elapsed_ms = int((time.monotonic() - started) * 1000)

    # Backtrace auto-decode — only attempts if --project was provided.
    decoded_backtrace = bt.decoded_to_jsonable(
        bt.decode_text(result.text, project_dir=args.project)
    ) if args.project else []

    payload = {
        "port": port,
        "baud": args.baud,
        "elapsed_ms": elapsed_ms,
        "n_lines": len(result.lines),
        "n_bytes": len(result.text.encode("utf-8")),
        "matched": result.matched_pattern,
        "timed_out": result.timed_out,
        "text": result.text,
        "lines": result.lines,
        "decoded_backtrace": decoded_backtrace,
    }

    if args.require_match and args.until and not result.matched_pattern:
        output.failure(
            exit_code=MONITOR_TIMEOUT,
            error=f"Timed out after {args.seconds}s without matching /{args.until}/",
            details=payload,
        )
        return MONITOR_TIMEOUT

    if output.json_mode:
        output.success(payload)
    else:
        # human mode: we already streamed lines via on_line; just summarise.
        match_str = f" (matched /{result.matched_pattern}/)" if result.matched_pattern else ""
        print(f"\n[captured {len(result.lines)} lines in {elapsed_ms/1000:.1f}s{match_str}]",
              flush=True)
    return OK
