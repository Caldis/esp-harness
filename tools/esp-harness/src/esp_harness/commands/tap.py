"""`esp-harness tap` — send tap signal(s) to firmware over UART.

Pairs with a firmware-side listener that synthesises a touch event whenever
any byte arrives on USB-Serial/JTAG. See image-viewer's `serial_tap_task`
in main/image_viewer_main.c for the canonical implementation.

Each `tap` writes a single newline byte. Use `--count N --interval-ms M` for
multi-tap sequences.
"""

from __future__ import annotations

import argparse
import time

try:
    import serial  # type: ignore[import-untyped]
except ImportError as e:
    raise ImportError("pyserial is required. pip install pyserial") from e

from esp_harness.core import ports as ports_mod
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
        "tap",
        help="Send tap byte(s) to firmware (pairs with serial_tap_task on the device).",
    )
    p.add_argument("--port", default=None, help="COM port (auto-detect if omitted).")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--count", type=int, default=1, help="Number of taps (default: 1).")
    p.add_argument(
        "--interval-ms",
        type=int,
        default=300,
        help="Spacing between taps in ms (default: 300).",
    )
    p.add_argument(
        "--byte",
        default="\n",
        help="Byte to send per tap (default: newline). Pass exactly one character.",
    )
    p.add_argument(
        "--at",
        default=None,
        metavar="X,Y",
        help="Coordinate-precise tap. Sends 'tap X Y\\n' (a console-protocol "
             "command) instead of a single byte. Requires firmware that "
             "implements the 'tap X Y' console command (see esp32-harness-"
             "showcase's harness_commands.c).",
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

    # --at X,Y switches us from "send raw byte(s)" to "send 'tap X Y\n'
    # console command". Both modes go through the same serial open path.
    if args.at is not None:
        try:
            x_str, y_str = args.at.split(",", 1)
            x = int(x_str.strip())
            y = int(y_str.strip())
        except (ValueError, AttributeError):
            output.failure(
                exit_code=2,
                error=f"--at must be 'X,Y' (got {args.at!r})",
            )
            return 2
        payload = f"tap {x} {y}\n".encode("utf-8")
        mode_desc = f"coord ({x},{y})"
    else:
        if len(args.byte) != 1:
            output.failure(
                exit_code=2,
                error="--byte must be exactly one character.",
                details={"got": args.byte},
            )
            return 2
        payload = args.byte.encode("utf-8")
        mode_desc = f"byte {args.byte!r}"

    if args.count < 1:
        output.failure(exit_code=2, error="--count must be >= 1.")
        return 2

    output.info(f"tap {port} {mode_desc} x{args.count} (interval {args.interval_ms}ms)")
    started = time.monotonic()

    try:
        # Suppress the implicit DTR/RTS pulse that pyserial does on
        # open() — that pulse resets the ESP32-S3 (native USB-Serial/JTAG
        # treats DTR transitions as a reset signal). Same dance as
        # core/serial_io.py.
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = args.baud
        ser.timeout = 0.5
        ser.write_timeout = 1.0
        ser.dtr = False
        ser.rts = False
        ser.open()
        try:
            ser.dtr = False
            ser.rts = False
        except Exception:
            pass
    except Exception as e:
        msg = str(e)
        if "access" in msg.lower() or "permission" in msg.lower() or "busy" in msg.lower():
            output.failure(
                exit_code=DEVICE_BUSY,
                error=f"Port {port} busy: {e}",
                details={"port": port},
                human="Close any open monitor or terminal on this port first.",
            )
            return DEVICE_BUSY
        output.failure(
            exit_code=GENERIC_ERROR,
            error=f"Failed to open {port}: {e}",
            details={"port": port},
        )
        return GENERIC_ERROR

    try:
        for i in range(args.count):
            ser.write(payload)
            ser.flush()
            output.debug(f"sent tap {i+1}/{args.count}")
            if i < args.count - 1:
                time.sleep(args.interval_ms / 1000.0)
    finally:
        try:
            ser.close()
        except Exception:
            pass

    elapsed_ms = int((time.monotonic() - started) * 1000)
    output.success(
        {
            "port": port,
            "baud": args.baud,
            "count": args.count,
            "interval_ms": args.interval_ms,
            "mode": "coord" if args.at else "byte",
            "at": args.at,
            "byte": args.byte if args.at is None else None,
            "elapsed_ms": elapsed_ms,
        },
        human=f"sent {args.count} {mode_desc} tap(s) to {port} in {elapsed_ms}ms",
    )
    return OK
