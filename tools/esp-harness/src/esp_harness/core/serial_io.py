"""Non-interactive serial capture.

Read from a serial port for at most N seconds, or until a regex pattern matches,
whichever comes first. This is the primitive the `monitor` command builds on,
and the AI loop's "see what the device said" mechanism.

Critically: no terminal, no user input, no hanging. Open → drain → close → return.
"""

from __future__ import annotations

import re
import time
from dataclasses import dataclass

try:
    import serial  # type: ignore[import-untyped]
except ImportError as e:
    raise ImportError("pyserial is required. pip install pyserial") from e


@dataclass
class CaptureResult:
    text: str
    lines: list[str]
    elapsed_s: float
    matched_pattern: str | None  # regex source that triggered early exit, if any
    timed_out: bool  # True if we hit the timeout before matching anything


def capture(
    port: str,
    *,
    baud: int = 115200,
    seconds: float = 30.0,
    until: str | None = None,
    open_timeout: float = 5.0,
    poll_interval: float = 0.05,
    on_line=None,             # optional callback(str) -> None for live tee
    inject_bytes: bytes | None = None,  # write once just after opening
    inject_delay_ms: int = 100,         # short pause after open before injecting
    inject_repeat: int = 1,             # send the same bytes this many times
    inject_repeat_interval_ms: int = 500,  # gap between repeats
    no_reset: bool = True,              # keep DTR/RTS deasserted on open
) -> CaptureResult:
    """Open `port`, drain bytes for up to `seconds`, or until `until` regex matches.

    `until` is matched against accumulated text after each chunk, so multi-line
    patterns work (use `re.DOTALL`-friendly syntax if needed).

    If `inject_bytes` is given, those bytes are written once shortly after the
    port opens. This is used by the AI loop to synthesise a tap inside the
    same monitor session, since Windows COM ports are exclusive — separate
    tap + monitor processes can't share the device.

    Raises:
        serial.SerialException: port can't be opened (busy, missing, etc.)
    """
    pattern: re.Pattern[str] | None = re.compile(until) if until else None

    deadline = time.monotonic() + seconds

    try:
        # We construct the Serial object without opening it so we can set
        # DTR/RTS to False FIRST. If we let pyserial open with default DTR
        # asserted, the ESP32-S3's native USB-Serial/JTAG interprets the DTR
        # transition as a reset trigger and the device reboots — destroying
        # our state across consecutive monitor sessions.
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = baud
        ser.timeout = poll_interval
        ser.write_timeout = 0.5
        if no_reset:
            ser.dtr = False
            ser.rts = False
        ser.open()
        # Belt-and-suspenders: some Windows drivers ignore the pre-open
        # setting and re-assert on open. Force them off again.
        if no_reset:
            try:
                ser.dtr = False
                ser.rts = False
            except Exception:
                pass
    except Exception:
        raise

    accumulated = ""
    lines: list[str] = []
    line_buf = ""
    matched: str | None = None
    timed_out = False
    injected = False

    start = time.monotonic()
    next_inject_at = (start + inject_delay_ms / 1000.0) if inject_bytes else None
    injections_remaining = inject_repeat if inject_bytes else 0
    try:
        while time.monotonic() < deadline:
            # Repeatable byte injection (fake taps into the firmware listener).
            if (inject_bytes is not None
                    and injections_remaining > 0
                    and next_inject_at is not None
                    and time.monotonic() >= next_inject_at):
                try:
                    ser.write(inject_bytes)
                    ser.flush()
                except Exception:
                    pass
                injections_remaining -= 1
                injected = True
                if injections_remaining > 0:
                    next_inject_at = time.monotonic() + inject_repeat_interval_ms / 1000.0
                else:
                    next_inject_at = None
            chunk = ser.read(4096)  # up to N bytes, returns after timeout
            if chunk:
                text = chunk.decode("utf-8", errors="replace")
                accumulated += text
                line_buf += text
                # split on newline, emit complete lines
                while "\n" in line_buf:
                    one, line_buf = line_buf.split("\n", 1)
                    one = one.rstrip("\r")
                    lines.append(one)
                    if on_line is not None:
                        on_line(one)

                if pattern is not None and pattern.search(accumulated):
                    matched = pattern.pattern
                    break
        else:
            timed_out = pattern is not None  # only "timed out" if we were waiting for a pattern

        # flush trailing partial line
        if line_buf:
            lines.append(line_buf.rstrip("\r"))
            if on_line is not None:
                on_line(line_buf.rstrip("\r"))
            accumulated_text = accumulated
        else:
            accumulated_text = accumulated
    finally:
        try:
            ser.close()
        except Exception:
            pass

    elapsed = time.monotonic() - start
    return CaptureResult(
        text=accumulated_text,
        lines=lines,
        elapsed_s=elapsed,
        matched_pattern=matched,
        timed_out=timed_out,
    )
