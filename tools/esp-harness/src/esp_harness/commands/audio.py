"""`esp-harness audio` — speaker / mic console wrapper with a safety cap.

The on-board ES8311 → NS-class-D → speaker chain is uncomfortably loud
above ~30 % volume during repeated test runs. This wrapper caps the
`tone` and `loopback` volumes to 30 by default and prints a warning if
the caller asks for more. GUI use (Listen scene + BOOT/USER volume
buttons) is unaffected — only the CLI/automation path goes through
this clamp.

Subcommands:
    audio tone FREQ_HZ [DUR_MS] [--vol N]
    audio mic [DUR_MS]
    audio loopback [DUR_MS]
    audio vol [N]
    audio diag [--force]
"""

from __future__ import annotations

import argparse
import json
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


# Hard cap for any speaker-output volume the toolkit is allowed to set.
# Picked after the human user pointed out that earlier tests at default
# 60 % and 95 % were physically uncomfortable. The cap is enforced on
# the host side specifically so it survives any firmware change.
VOL_CAP = 30


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser(
        "audio",
        help=f"Speaker/mic console commands (volume capped at {VOL_CAP}%% for hearing safety).",
    )
    p.add_argument("--port", default=None, help="COM port (auto-detect if omitted).")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument(
        "--reply-timeout",
        type=float,
        default=4.0,
        help="Seconds to wait for the OK:/ERR: response (default 4.0).",
    )

    audio_sub = p.add_subparsers(dest="audio_sub", metavar="<sub>")
    audio_sub.required = True

    tone = audio_sub.add_parser(
        "tone",
        help=f"Play a sine tone. VOL is clamped to {VOL_CAP}%% even if caller asks more.",
    )
    tone.add_argument("freq", type=int, help="Tone frequency in Hz (1..22050).")
    tone.add_argument(
        "duration_ms", nargs="?", type=int, default=400,
        help="Tone duration in ms (default 400; firmware caps at 5000).",
    )
    tone.add_argument(
        "--vol", type=int, default=VOL_CAP,
        help=f"Volume 0..100; clamped to {VOL_CAP} (toolkit hearing-safety cap).",
    )

    mic = audio_sub.add_parser("mic", help="Capture mic, return peak + RMS dBFS.")
    mic.add_argument("duration_ms", nargs="?", type=int, default=1000)

    lb = audio_sub.add_parser("loopback", help="Record + play back (volume also capped).")
    lb.add_argument("duration_ms", nargs="?", type=int, default=1000)

    vol = audio_sub.add_parser("vol", help="Get or set firmware speaker volume.")
    vol.add_argument(
        "value", nargs="?", type=int, default=None,
        help=f"New volume 0..100; clamped to {VOL_CAP} if set.",
    )

    diag = audio_sub.add_parser(
        "diag", help="PA pin diagnostic. --force drives GPIO 46 HIGH manually."
    )
    diag.add_argument("--force", action="store_true")

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


def _open_serial(port: str, baud: int) -> "serial.Serial":
    """Open without DTR/RTS reset pulse — ESP32-S3 native USB-JTAG treats
    DTR transitions as a reset signal."""
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.1
    ser.write_timeout = 1.0
    ser.dtr = False
    ser.rts = False
    ser.open()
    try:
        ser.dtr = False
        ser.rts = False
    except Exception:
        pass
    return ser


def _exchange(ser: "serial.Serial", line: str, timeout: float) -> tuple[str | None, str | None]:
    """Send one command line, return (ok_payload, err_payload). Exactly
    one is non-None on a successful exchange. Either may be None on
    timeout (returned as a special tuple)."""
    ser.reset_input_buffer()
    ser.write((line + "\n").encode("utf-8"))
    ser.flush()
    deadline = time.monotonic() + timeout
    buf = b""
    while time.monotonic() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            text = buf.decode("utf-8", "replace")
            for raw in text.splitlines():
                if raw.startswith("OK:"):
                    return raw[3:].strip(), None
                if raw.startswith("ERR:"):
                    return None, raw[4:].strip()
        time.sleep(0.02)
    return None, None


def run(args: argparse.Namespace, output: Output) -> int:
    port, code = _resolve_port(args.port, output)
    if port is None:
        return code

    sub = args.audio_sub

    # --- volume clamping happens here, before we ever write to serial ---
    capped_warning: str | None = None
    if sub == "tone":
        requested_vol = args.vol
        if requested_vol > VOL_CAP:
            capped_warning = (
                f"--vol {requested_vol} requested, clamped to {VOL_CAP} "
                f"(toolkit hearing-safety cap). Use the GUI BOOT/USER "
                f"buttons in Scene XII to go louder."
            )
            output.info(capped_warning)
            requested_vol = VOL_CAP
        line = f"audio tone {args.freq} {args.duration_ms} {requested_vol}"
    elif sub == "mic":
        line = f"audio mic {args.duration_ms}"
    elif sub == "loopback":
        # Also clamp the persistent firmware volume before loopback —
        # the device's current setting could be anything. We set it
        # explicitly, run loopback, then restore? Restoring would need
        # an extra round-trip. Simpler: just push down to cap and
        # leave it; the user can crank it back up via the GUI.
        try:
            _ensure_volume_under_cap(port, args.baud, args.reply_timeout, output)
        except _SerialBusy as e:
            output.failure(exit_code=DEVICE_BUSY, error=str(e))
            return DEVICE_BUSY
        line = f"audio loopback {args.duration_ms}"
    elif sub == "vol":
        if args.value is None:
            line = "audio vol"
        else:
            v = args.value
            if v > VOL_CAP:
                capped_warning = (
                    f"audio vol {v} requested, clamped to {VOL_CAP}."
                )
                output.info(capped_warning)
                v = VOL_CAP
            line = f"audio vol {v}"
    elif sub == "diag":
        line = "audio diag force" if args.force else "audio diag"
    else:
        output.failure(exit_code=2, error=f"unknown audio sub '{sub}'")
        return 2

    # --- send the (clamped) command line and read response ---
    try:
        ser = _open_serial(port, args.baud)
    except Exception as e:
        msg = str(e)
        if any(k in msg.lower() for k in ("access", "permission", "busy")):
            output.failure(
                exit_code=DEVICE_BUSY,
                error=f"Port {port} busy: {e}",
                human="Close any open monitor on this port first.",
            )
            return DEVICE_BUSY
        output.failure(exit_code=GENERIC_ERROR, error=f"Failed to open {port}: {e}")
        return GENERIC_ERROR

    try:
        ok_payload, err_payload = _exchange(ser, line, args.reply_timeout)
    finally:
        try:
            ser.close()
        except Exception:
            pass

    if ok_payload is None and err_payload is None:
        output.failure(
            exit_code=GENERIC_ERROR,
            error=f"no reply to '{line}' within {args.reply_timeout}s",
        )
        return GENERIC_ERROR
    if err_payload is not None:
        output.failure(
            exit_code=GENERIC_ERROR, error=f"firmware ERR: {err_payload}"
        )
        return GENERIC_ERROR

    # ok_payload is whatever followed "OK:" — typically a JSON blob.
    parsed: object = ok_payload
    try:
        parsed = json.loads(ok_payload)
    except Exception:
        pass

    output.success(
        {
            "port": port,
            "sent": line,
            "vol_cap": VOL_CAP,
            "warning": capped_warning,
            "reply": parsed,
        },
        human=f"{line} → OK",
    )
    return OK


class _SerialBusy(Exception):
    pass


def _ensure_volume_under_cap(port: str, baud: int, timeout: float, output: Output) -> None:
    """Push firmware volume down to VOL_CAP if it's currently higher.
    Used before `audio loopback` so the playback at the end can't blast
    the user. Best-effort; failure is logged but doesn't abort the
    main command."""
    try:
        ser = _open_serial(port, baud)
    except Exception as e:
        raise _SerialBusy(f"Port {port} busy while pre-capping volume: {e}") from e
    try:
        ok, _ = _exchange(ser, "audio vol", timeout)
        cur = None
        if ok:
            try:
                cur = int(json.loads(ok).get("volume"))
            except Exception:
                pass
        if cur is None or cur > VOL_CAP:
            _exchange(ser, f"audio vol {VOL_CAP}", timeout)
            output.info(f"clamped firmware volume to {VOL_CAP} before loopback "
                        f"(was {cur if cur is not None else 'unknown'}).")
    finally:
        try:
            ser.close()
        except Exception:
            pass
