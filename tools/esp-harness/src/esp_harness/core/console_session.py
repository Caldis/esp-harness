"""Console session helper — speaks the line-based protocol with the device.

The device-side firmware (see esp32-harness-showcase's
`main/harness/console_protocol.c`) accepts newline-delimited commands and
responds with `OK:`, `ERR:`, or `EVT:` prefixed lines. Multi-line binary
payloads are framed with `<TAG>_BEGIN <meta>\\n...\\n<TAG>_END\\n`.

This module wraps that protocol over pyserial with the standard
no-reset DTR/RTS handling we use everywhere.
"""

from __future__ import annotations

import re
import time
from dataclasses import dataclass, field
from typing import Iterator

try:
    import serial  # type: ignore[import-untyped]
except ImportError as e:
    raise ImportError("pyserial is required. pip install pyserial") from e


_OK_RE  = re.compile(r"^OK:\s*(.*)$")
_ERR_RE = re.compile(r"^ERR:\s*(.*)$")
_EVT_RE = re.compile(r"^EVT:\s*(.*)$")
_BEGIN_RE = re.compile(r"^([A-Z][A-Z0-9_]*)_BEGIN\s*(.*)$")
_END_RE = re.compile(r"^([A-Z][A-Z0-9_]*)_END\s*$")


@dataclass
class Response:
    """The result of one command exchange."""
    ok: bool
    text: str = ""           # body after 'OK:' / 'ERR:' on the first reply line
    payload_meta: str = ""   # the line right after BEGIN tag (key=val pairs)
    payload: bytes = b""     # raw body bytes between BEGIN and END
    events: list[str] = field(default_factory=list)
    other_lines: list[str] = field(default_factory=list)
    raw: str = ""            # everything we observed, joined


class ConsoleSession:
    """Opens a serial port, sends a command, collects the reply.

    Usage:
        with ConsoleSession("COM9") as s:
            r = s.send("?ping")        # → Response(ok=True, text="pong")
            r = s.send("?dump w=128")  # → Response(ok=True, payload=b"...")
    """

    def __init__(
        self,
        port: str,
        *,
        baud: int = 115200,
        read_timeout: float = 0.1,
        write_timeout: float = 1.0,
        no_reset: bool = True,
        settle_ms: int = 50,
    ):
        self.port_name = port
        self.baud = baud
        self.read_timeout = read_timeout
        self.write_timeout = write_timeout
        self.no_reset = no_reset
        self.settle_ms = settle_ms
        self._ser: serial.Serial | None = None
        self._line_buf = ""

    def __enter__(self) -> "ConsoleSession":
        ser = serial.Serial()
        ser.port = self.port_name
        ser.baudrate = self.baud
        ser.timeout = self.read_timeout
        ser.write_timeout = self.write_timeout
        if self.no_reset:
            ser.dtr = False
            ser.rts = False
        ser.open()
        if self.no_reset:
            try:
                ser.dtr = False
                ser.rts = False
            except Exception:
                pass
        self._ser = ser
        # give the device some idle time so any in-flight EVT lines flush
        time.sleep(self.settle_ms / 1000.0)
        # discard whatever was already buffered
        try:
            ser.reset_input_buffer()
        except Exception:
            pass
        return self

    def __exit__(self, *_):
        if self._ser is not None:
            try:
                self._ser.close()
            except Exception:
                pass
            self._ser = None

    # ── low-level line reader ────────────────────────────────────────
    def _iter_lines(self, deadline: float) -> Iterator[str]:
        assert self._ser is not None
        while time.monotonic() < deadline:
            chunk = self._ser.read(4096)
            if chunk:
                text = chunk.decode("utf-8", errors="replace")
                self._line_buf += text
                while "\n" in self._line_buf:
                    one, self._line_buf = self._line_buf.split("\n", 1)
                    yield one.rstrip("\r")
            else:
                # idle tick — give caller a chance to break
                yield "\0"  # sentinel

    # ── send + collect ───────────────────────────────────────────────
    def send(
        self,
        cmd: str,
        *,
        timeout: float = 5.0,
        expect_payload: str | None = None,
    ) -> Response:
        """Send `cmd` (newline appended), read until OK:/ERR: arrives.

        If `expect_payload` is given, the OK reply is followed by a
        `<TAG>_BEGIN ... <TAG>_END` payload block; the bytes between are
        collected and returned in `Response.payload`.
        """
        assert self._ser is not None
        # send
        line = (cmd.rstrip("\r\n") + "\n").encode("utf-8")
        self._ser.write(line)
        self._ser.flush()

        # collect
        deadline = time.monotonic() + timeout
        resp = Response(ok=False)
        in_payload = False
        payload_tag = expect_payload
        raw_lines: list[str] = []

        for ln in self._iter_lines(deadline):
            if ln == "\0":  # idle
                continue
            raw_lines.append(ln)

            if in_payload:
                m_end = _END_RE.match(ln)
                if m_end and (payload_tag is None or m_end.group(1) == payload_tag):
                    in_payload = False
                    # consume any trailing newline already handled
                    break
                # body line (base64 or ASCII)
                resp.payload += ln.encode("ascii", errors="replace") + b"\n"
                continue

            m = _OK_RE.match(ln)
            if m:
                resp.ok = True
                resp.text = m.group(1)
                if expect_payload is None:
                    break
                # else continue to look for BEGIN
                continue

            m = _ERR_RE.match(ln)
            if m:
                resp.ok = False
                resp.text = m.group(1)
                break

            m = _EVT_RE.match(ln)
            if m:
                resp.events.append(m.group(1))
                continue

            m_begin = _BEGIN_RE.match(ln)
            if m_begin and (payload_tag is None or m_begin.group(1) == payload_tag):
                in_payload = True
                payload_tag = m_begin.group(1)
                resp.payload_meta = m_begin.group(2)
                continue

            # uncategorised line (could be log lines from ESP_LOG)
            resp.other_lines.append(ln)

        resp.raw = "\n".join(raw_lines)
        return resp
