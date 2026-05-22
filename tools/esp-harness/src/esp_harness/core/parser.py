"""Python reference implementation of the firmware console tokeniser
plus host-side helpers for reading the multi-line reply forms.

Two surfaces live here:

1. :func:`tokenise_console_line` — the canonical Python port of
   ``components/aurora-harness/src/console_protocol.c::dispatch_line``.
   Consumer projects (mocks, test rigs, host-side bridges) parse console
   lines exactly the way the device will, without copying the C
   algorithm. The parity test in ``tests/test_parser.py`` pins the two
   implementations together — when the C tokeniser changes (e.g. the
   G-7 fix at ``esp-harness@664b14e``), this file must be updated in
   the same commit. Otherwise mocks drift silently from the firmware
   (gap G-8 in the agent-dashboard project).

2. :class:`PayloadFollowsReader` — a host-side state machine that
   consumes the iterator of OK / ERR / EVT / TAG_BEGIN / inner /
   TAG_END lines emitted by the device, and yields a sequence of
   :class:`ReplyEvent` objects (``ok`` / ``err`` / ``evt`` /
   ``payload`` / ``log``). Bridges and tests reuse identical logic
   instead of rolling per-consumer state machines (gap G-H1).

Usage::

    from esp_harness.core.parser import tokenise_console_line, MAX_LINE
    argv = tokenise_console_line('dash snapshot "{\\"id\\":\\"req_1\\"}"')
    # → ['dash', 'snapshot', '{"id":"req_1"}']

    from esp_harness.core.parser import PayloadFollowsReader
    reader = PayloadFollowsReader()
    for evt in reader.feed_lines(iter_lines):
        if evt.kind == "payload":
            handle_blob(evt.tag, evt.blob)
        elif evt.kind == "err":
            log_warning(evt.text)
        ...
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Iterable, Iterator, Optional

MAX_ARGS = 8       # mirrors CONSOLE_MAX_ARGS in console_protocol.h
MAX_LINE = 1024    # mirrors CONSOLE_MAX_LINE in console_protocol.h


def tokenise_console_line(line: str) -> list[str]:
    """Split ``line`` into argv the way the device firmware will.

    Behaviour (1:1 with the C tokeniser):

    - Trailing ``\\r`` / space / tab are trimmed first.
    - Leading whitespace is skipped.
    - An empty (or all-whitespace) line returns ``[]``.
    - Tokens that LEAD with ``"`` preserve every inner ``"`` literally
      until the matching close-quote at end-of-token (whitespace or
      end-of-line). This is the G-7 contract that lets nested JSON
      payloads survive the parser.
    - Tokens that do NOT lead with ``"`` use the legacy
      toggle-on-any-quote behaviour (so ``wifi connect ssid="My AP"``
      still works).
    - At most :data:`MAX_ARGS` tokens are emitted; anything past that
      is silently discarded, just like the firmware.

    Note: the firmware applies an additional :data:`MAX_LINE` cap on the
    raw input *line* (oversize lines emit one ``ERR:`` and are drained).
    This function does not enforce that — it operates on whatever the
    caller chose to feed it. Callers that simulate the device wire
    contract should check ``len(line.encode()) < MAX_LINE`` first.
    """
    # Mirror dispatch_line's trailing-whitespace + leading-whitespace
    # preprocessing exactly. We use index walking instead of Python
    # string idioms so the line-for-line correspondence with the C
    # version stays obvious — when the C version changes, the diff
    # to this file should be readable.
    end = len(line)
    while end > 0 and line[end - 1] in ("\r", " ", "\t"):
        end -= 1
    p = 0
    while p < end and line[p] in (" ", "\t"):
        p += 1
    if p == end:
        return []

    argv: list[str] = []
    while p < end and len(argv) < MAX_ARGS:
        out: list[str] = []
        quoted_token = (line[p] == '"')
        in_quote = False
        if quoted_token:
            in_quote = True
            p += 1
        while p < end:
            ch = line[p]
            if not in_quote and ch in (" ", "\t"):
                break
            if ch == '"':
                if quoted_token:
                    nx = line[p + 1] if p + 1 < end else ""
                    if nx == "" or nx in (" ", "\t"):
                        in_quote = False
                        p += 1
                        break
                    # Embedded `"` inside a quote-leading token — keep it.
                    out.append(ch)
                    p += 1
                    continue
                # Legacy unquoted-token quote-toggle path.
                in_quote = not in_quote
                p += 1
                continue
            out.append(ch)
            p += 1
        argv.append("".join(out))
        while p < end and line[p] in (" ", "\t"):
            p += 1
    return argv


# ─────────────────────────────────────────────────────────────────────────────
# Host-side reply framing helper (G-H1)
# ─────────────────────────────────────────────────────────────────────────────

_OK_RE      = re.compile(r"^OK:\s*(.*)$")
_ERR_RE     = re.compile(r"^ERR:\s*(.*)$")
_EVT_RE     = re.compile(r"^EVT:\s*(.*)$")
_BEGIN_RE   = re.compile(r"^([A-Z][A-Z0-9_]*)_BEGIN\s*(.*)$")
_END_RE     = re.compile(r"^([A-Z][A-Z0-9_]*)_END\s*$")
# Self-describing OK lines from the firmware look like
#   OK: manifest follows tag=HELP
#   OK: payload follows tag=HEALTH
#   OK: dump start tag=DUMP w=466 h=466 ...
# We accept any text that contains `tag=<NAME>` and is followed by a
# TAG_BEGIN block on the wire. The match is tolerant to body variations.
_OK_TAG_RE  = re.compile(r"\btag=([A-Z][A-Z0-9_]*)\b")
# Backwards-compat trigger: `payload follows` / `manifest follows` /
# `dump start` without an explicit `tag=`. Older firmware emitted these
# before the G-4 self-describing-OK convention.
_OK_PAYLOAD_TRIGGERS = ("payload follows", "manifest follows", "dump start")


@dataclass
class ReplyEvent:
    """One semantic event produced by :class:`PayloadFollowsReader`.

    ``kind`` is one of:

    - ``"ok"``       — a single-line OK reply. ``text`` carries the body
                       after ``OK:``. ``tag`` is set when the OK was
                       self-describing (``OK: ... tag=HEALTH``) but no
                       payload block followed — rare but legal.
    - ``"err"``      — an ERR reply. ``text`` carries the body after
                       ``ERR:``. Bridges should surface or log these
                       (gap G-H3 — silent swallowing of ERR is a bug).
    - ``"evt"``      — an EVT line. ``text`` carries the body after
                       ``EVT:``.
    - ``"payload"``  — a payload-follows reply has completed. ``tag``
                       names the block (HEALTH, HELP, SCENES, DUMP…),
                       ``meta`` is the key=val string after the BEGIN
                       tag, and ``blob`` is the inner body (lines
                       joined by ``\\n``, no trailing newline).
    - ``"log"``      — an uncategorised line (ESP_LOG output and the
                       like). Bridges typically ignore these but tests
                       can use them to assert e.g. a warning fired.

    Consumers should ``match evt.kind:`` rather than rely on field
    presence — every event always has ``kind`` and ``text``; the
    payload/tag/meta/blob fields are populated only for the relevant
    kinds and default to empty string / ``None``.
    """
    kind: str            # "ok" | "err" | "evt" | "payload" | "log"
    text: str = ""       # body after the prefix (OK:/ERR:/EVT:) or full line for "log"
    tag: Optional[str] = None
    meta: str = ""       # key=val string from <TAG>_BEGIN
    blob: str = ""       # inner body for "payload"


class PayloadFollowsReader:
    """Consume a stream of console lines and yield semantic events.

    The device's reply protocol has four single-line forms — ``OK:``,
    ``ERR:``, ``EVT:``, and uncategorised log lines — plus one
    multi-line form: ``OK: ... follows tag=<TAG>`` followed by
    ``<TAG>_BEGIN <meta>\\n<body lines>\\n<TAG>_END``. Each consumer
    that wants to parse the multi-line form needs the same state
    machine. Before this helper, every bridge rolled its own (see the
    pre-v0.2 ``claude_buddy_bridge.py:DevicePusher._process_line``),
    each with subtly different edge cases.

    Usage::

        reader = PayloadFollowsReader()

        for line in iter_lines():           # whatever yields wire lines
            for evt in reader.feed(line):
                handle(evt)

        # Or, given an iterable, exhaust it eagerly:
        for evt in reader.feed_lines(line_iter):
            handle(evt)

    A single call to :meth:`feed` may yield ZERO events (we're mid-
    payload and just consumed an inner body line), ONE event (the
    common case — single-line OK/ERR/EVT), or be part of a multi-line
    payload that completes on a future call. Multiple events are not
    returned from a single ``feed()`` call.

    The reader is purely state-machine — it does not own a transport.
    Re-use across reconnects by calling :meth:`reset` (drops any
    in-progress payload block).

    Backwards compatibility: OK lines without an explicit ``tag=`` are
    still recognised as payload triggers if the body starts with
    ``payload follows`` / ``manifest follows`` / ``dump start``. In
    that case the tag is taken from the subsequent ``<TAG>_BEGIN``
    line. Post-G-4 firmware always sets ``tag=`` so consumers can
    pick the right ``--payload TAG`` without grepping; the legacy
    path keeps older devices working.
    """

    def __init__(self) -> None:
        self._await_tag: Optional[str] = None
        self._await_meta: str = ""
        self._await_buf: list[str] = []
        # If we saw `payload follows` without an explicit tag, we don't
        # know the tag name until <TAG>_BEGIN arrives. In that state we
        # accept the first BEGIN we see.
        self._await_tag_pending: bool = False
        # Whether we've already absorbed the <TAG>_BEGIN framing line
        # for the current in-progress payload (self-describing OK
        # paths don't know the BEGIN line; we recognise and skip it
        # the first time we see it).
        self._await_begin_seen: bool = False

    def reset(self) -> None:
        """Drop any in-progress payload state (e.g. after reconnect)."""
        self._await_tag = None
        self._await_meta = ""
        self._await_buf.clear()
        self._await_tag_pending = False
        self._await_begin_seen = False

    @property
    def in_payload(self) -> bool:
        """True if a payload block has started but not yet ended."""
        return self._await_tag is not None or self._await_tag_pending

    def feed(self, line: str) -> Iterator[ReplyEvent]:
        """Feed one line. Yields zero or one :class:`ReplyEvent`."""
        # We're mid-payload-body collection. The very first line after
        # the OK is the matching `<TAG>_BEGIN <meta>` framing line — we
        # skip it (and refresh `meta` from it; the BEGIN line carries
        # the canonical key=val pairs in the wire protocol). After
        # that, every line is body until <TAG>_END.
        if self._await_tag is not None:
            m_end = _END_RE.match(line)
            if m_end and m_end.group(1) == self._await_tag:
                evt = ReplyEvent(
                    kind="payload",
                    tag=self._await_tag,
                    meta=self._await_meta,
                    blob="\n".join(self._await_buf),
                )
                self.reset()
                yield evt
                return
            m_begin = _BEGIN_RE.match(line)
            if m_begin and m_begin.group(1) == self._await_tag \
                    and not self._await_buf and not self._await_begin_seen:
                # Skip the framing BEGIN line; merge its key=val pairs
                # into meta (the OK body and the BEGIN body each carry
                # useful fields — the OK has `tag=`, the BEGIN has
                # `fmt=` / `bytes=`).
                begin_meta = m_begin.group(2)
                if begin_meta:
                    self._await_meta = (
                        f"{self._await_meta} {begin_meta}".strip()
                        if self._await_meta else begin_meta
                    )
                self._await_begin_seen = True
                return
            self._await_buf.append(line)
            return

        # We saw "payload follows" without a tag; the next BEGIN line
        # tells us what tag it is.
        if self._await_tag_pending:
            m_begin = _BEGIN_RE.match(line)
            if m_begin:
                self._await_tag = m_begin.group(1)
                self._await_meta = m_begin.group(2)
                self._await_tag_pending = False
                self._await_begin_seen = True
                return
            # Anything else aborts the wait (firmware bug or out-of-
            # band log line). Surface it as a log event.
            self._await_tag_pending = False
            yield ReplyEvent(kind="log", text=line)
            return

        m = _OK_RE.match(line)
        if m:
            body = m.group(1)
            tag_match = _OK_TAG_RE.search(body)
            if tag_match:
                # Self-describing OK — the payload block follows. Wait
                # for the matching BEGIN.
                self._await_tag = tag_match.group(1)
                self._await_buf.clear()
                self._await_begin_seen = False
                # Stash the OK body as the initial meta. The BEGIN
                # line that follows will overwrite this with the more
                # canonical key=val string. Either way, consumers can
                # see e.g. `tag=DUMP w=466` in `evt.meta`.
                self._await_meta = body
                return
            # Legacy OK without tag: maybe the device is pre-G-4. If
            # the body starts with a known trigger phrase, wait for
            # the BEGIN to learn the tag.
            for trig in _OK_PAYLOAD_TRIGGERS:
                if body.startswith(trig):
                    self._await_tag_pending = True
                    return
            # Plain OK — emit immediately.
            yield ReplyEvent(kind="ok", text=body)
            return

        m = _ERR_RE.match(line)
        if m:
            yield ReplyEvent(kind="err", text=m.group(1))
            return

        m = _EVT_RE.match(line)
        if m:
            yield ReplyEvent(kind="evt", text=m.group(1))
            return

        # A BEGIN line without a preceding OK is malformed in the
        # current protocol; emit it as a log entry so test rigs can
        # assert on it, but do not start a payload (we don't have a
        # legitimate OK to anchor it to).
        m_begin = _BEGIN_RE.match(line)
        if m_begin:
            yield ReplyEvent(kind="log", text=line)
            return

        # Uncategorised (ESP_LOG, status, etc.)
        yield ReplyEvent(kind="log", text=line)

    def feed_lines(self, lines: Iterable[str]) -> Iterator[ReplyEvent]:
        """Convenience: feed an iterable, yield all events in order."""
        for ln in lines:
            yield from self.feed(ln)
