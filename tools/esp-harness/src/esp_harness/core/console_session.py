"""Console session helper — speaks the line-based protocol with the device.

The device-side firmware (see
`components/aurora-harness/src/console_protocol.c` for the implementation
and `examples/aurora/main/harness/harness_commands.c` for a consumer)
accepts newline-delimited commands and responds with `OK:`, `ERR:`, or
`EVT:` prefixed lines. Multi-line binary
payloads are framed with `<TAG>_BEGIN <meta>\\n...\\n<TAG>_END\\n`.

This module wraps that protocol over pyserial with the standard
no-reset DTR/RTS handling we use everywhere.
"""

from __future__ import annotations

import re
import time
from dataclasses import dataclass, field
from typing import Callable, Iterator, Optional

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
    matched_evt: str | None = None   # the EVT body that matched wait_evt (if any)
    evt_wait_ms: int = 0     # ms actually spent waiting for the named EVT
    # G-H3 — every ERR line observed during the exchange (the first one
    # also lands in `text` when `ok=False`, but subsequent ERRs were
    # previously dropped). Bridges that need to surface device errors
    # iterate this list. Empty when no ERR arrived.
    errs: list[str] = field(default_factory=list)


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
        on_err: Optional[Callable[[str], None]] = None,
    ):
        self.port_name = port
        self.baud = baud
        self.read_timeout = read_timeout
        self.write_timeout = write_timeout
        self.no_reset = no_reset
        self.settle_ms = settle_ms
        self._ser: serial.Serial | None = None
        self._line_buf = ""
        # G-H3 — invoked with the ERR body every time an ``ERR:`` line
        # is seen during ``send()``. The agent-dashboard's pre-v0.2
        # bridge surfaced this gap: ``send()`` swallowed everything
        # after the first OK or the first ERR — so if a SECOND ERR
        # arrived (e.g. the device emitted an async error during a
        # snapshot stream), the host saw nothing. With this hook the
        # caller can log or raise. Backward-compatible: ``None`` keeps
        # the old behaviour (ERRs only land in ``Response.text``).
        self.on_err = on_err

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
        wait_evt: str | None = None,
        evt_timeout: float = 2.0,
    ) -> Response:
        """Send `cmd` (newline appended), read until OK:/ERR: arrives.

        If `expect_payload` is given, the OK reply is followed by a
        `<TAG>_BEGIN ... <TAG>_END` payload block; the bytes between are
        collected and returned in `Response.payload`.

        If `wait_evt` is given (regex pattern), the session keeps reading
        for up to `evt_timeout` additional seconds AFTER the OK:/ERR: line,
        scanning incoming EVT lines for a body matching the pattern.
        First match populates `Response.matched_evt` and ends the read.
        Use this for async commands whose payload lands as an EVT after
        the synchronous acknowledgement (e.g. `tap` → `tap_hit`,
        `?ota download` → `OTA progress=...`).
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
        ack_seen = False  # OK:/ERR: arrived
        evt_re = re.compile(wait_evt) if wait_evt else None
        evt_wait_start: float | None = None

        for ln in self._iter_lines(deadline):
            if ln == "\0":  # idle
                # Once we have the ack and are only waiting for the EVT,
                # check whether the EVT deadline has elapsed even when
                # no new bytes arrived.
                if ack_seen and evt_re is not None and evt_wait_start is not None:
                    if time.monotonic() - evt_wait_start >= evt_timeout:
                        break
                continue
            raw_lines.append(ln)

            if in_payload:
                m_end = _END_RE.match(ln)
                if m_end and (payload_tag is None or m_end.group(1) == payload_tag):
                    in_payload = False
                    # consume any trailing newline already handled
                    if evt_re is None:
                        break
                    # Otherwise fall through — we still need to wait for EVT.
                    continue
                # body line (base64 or ASCII)
                resp.payload += ln.encode("ascii", errors="replace") + b"\n"
                continue

            m = _OK_RE.match(ln)
            if m:
                resp.ok = True
                resp.text = m.group(1)
                ack_seen = True
                # If we're not waiting for anything else, stop now.
                if expect_payload is None and evt_re is None:
                    break
                # If the EVT we were waiting for already arrived
                # *before* OK (commands that emit progress during
                # execution — e.g. `scene next` → `scene_changed`
                # fires from the scene-fw notify before cmd_scene's
                # console_reply_ok), we don't need to wait any further.
                if evt_re is not None and resp.matched_evt is not None \
                   and expect_payload is None:
                    break
                # else continue: look for BEGIN and/or wait for EVT.
                if evt_re is not None and resp.matched_evt is None:
                    # Switch to the (usually shorter) EVT-only deadline so
                    # we don't sit on the parent timeout for no reason.
                    evt_wait_start = time.monotonic()
                    deadline = min(deadline, evt_wait_start + evt_timeout)
                continue

            m = _ERR_RE.match(ln)
            if m:
                err_body = m.group(1)
                resp.errs.append(err_body)
                if self.on_err is not None:
                    try:
                        self.on_err(err_body)
                    except Exception:
                        # A misbehaving callback must not break the
                        # session — the bridge will see the ERR in
                        # `resp.errs` either way.
                        pass
                # Only the FIRST ERR populates `text` + `ok=False` so
                # we don't churn the legacy single-shot contract. The
                # full ERR history is in `resp.errs` (G-H3).
                if not ack_seen:
                    resp.ok = False
                    resp.text = err_body
                    ack_seen = True
                # ERR short-circuits: an async EVT after an error is rare
                # and the caller probably wants to know about the failure
                # immediately. If a real use case for waiting after ERR
                # shows up, add a flag.
                break

            m = _EVT_RE.match(ln)
            if m:
                body = m.group(1)
                resp.events.append(body)
                # Test the regex on EVERY EVT, not just post-ack ones.
                # Some firmware commands (e.g. `scene next`) emit the
                # outcome EVT during command processing — BEFORE the
                # synchronous OK: reply. Gating on ack_seen here meant
                # those EVTs landed in `events` but never matched.
                if evt_re is not None and resp.matched_evt is None:
                    if evt_re.search(body):
                        resp.matched_evt = body
                        if ack_seen:
                            # Match arrived AFTER ack — measure from
                            # the wait_start time. If ack hasn't arrived
                            # yet, evt_wait_ms stays 0 (we matched
                            # pre-ack and didn't need to wait).
                            if evt_wait_start is not None:
                                resp.evt_wait_ms = int(
                                    (time.monotonic() - evt_wait_start) * 1000)
                            # Post-ack match — safe to stop reading.
                            break
                        # Pre-ack match: keep reading until OK: arrives
                        # so the caller still gets `resp.ok` set
                        # correctly. The break above on ack_seen handles
                        # the early-exit path.
                continue

            m_begin = _BEGIN_RE.match(ln)
            if m_begin and (payload_tag is None or m_begin.group(1) == payload_tag):
                in_payload = True
                payload_tag = m_begin.group(1)
                resp.payload_meta = m_begin.group(2)
                continue

            # uncategorised line (could be log lines from ESP_LOG)
            resp.other_lines.append(ln)

        # On timeout / no-match, record how long we actually waited for
        # the EVT. Without this evt_wait_ms stayed 0 in both "matched
        # instantly" and "waited 2s then gave up" cases — undistinguishable
        # by an AI agent reading the JSON.
        if evt_re is not None and resp.matched_evt is None \
                and evt_wait_start is not None:
            resp.evt_wait_ms = int(
                (time.monotonic() - evt_wait_start) * 1000)

        resp.raw = "\n".join(raw_lines)
        return resp
