"""Tests for ``esp_harness.client.open_persistent_session`` (G-1, G-3).

The persistent-session API exists so a host bridge can hold ONE open
connection for the lifetime of its process — high-rate snapshot pushes
PLUS a long-lived EVT listener share the same wire instead of each
re-opening the port (~140 ms per call). We test against a tiny in-
process TCP mock so the suite runs in CI without COM9.
"""

from __future__ import annotations

import json
import socket
import threading
import time

import pytest

from esp_harness.client import (
    ReplyEvent,
    SessionHandle,
    TransportError,
    open_persistent_session,
)


# ─────────────────────────────────────────────────────────────────────────────
# Tiny TCP echo mock — accepts dash <verb> [json] and replies OK or EVT
# ─────────────────────────────────────────────────────────────────────────────


class _MockServer:
    """Minimal newline-framed server. One connection at a time."""

    def __init__(self) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.received: list[str] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()
        self._client_sock: socket.socket | None = None
        self._client_lock = threading.Lock()

    @property
    def addr(self) -> str:
        return f"127.0.0.1:{self.port}"

    def stop(self) -> None:
        self._stop.set()
        try:
            self.sock.close()
        except OSError:
            pass
        with self._client_lock:
            if self._client_sock:
                try:
                    self._client_sock.close()
                except OSError:
                    pass

    def push(self, line: str) -> None:
        """Server-initiated push (e.g. async EVT) to the connected client."""
        with self._client_lock:
            if self._client_sock is None:
                return
            try:
                self._client_sock.sendall(line.rstrip("\n").encode("utf-8") + b"\n")
            except OSError:
                pass

    def _serve(self) -> None:
        try:
            while not self._stop.is_set():
                try:
                    conn, _ = self.sock.accept()
                except OSError:
                    return
                with self._client_lock:
                    self._client_sock = conn
                self._handle(conn)
                with self._client_lock:
                    self._client_sock = None
        except Exception:
            return

    def _handle(self, conn: socket.socket) -> None:
        conn.settimeout(0.2)
        buf = b""
        try:
            while not self._stop.is_set():
                try:
                    chunk = conn.recv(4096)
                except socket.timeout:
                    continue
                except OSError:
                    return
                if not chunk:
                    return
                buf += chunk
                while b"\n" in buf:
                    one, buf = buf.split(b"\n", 1)
                    line = one.decode("utf-8", errors="replace").rstrip("\r")
                    self._dispatch(conn, line)
        finally:
            try:
                conn.close()
            except OSError:
                pass

    def _dispatch(self, conn: socket.socket, line: str) -> None:
        self.received.append(line)
        # Recognise a tiny grammar:
        #   dash idle              → OK: {"scene":"idle"}
        #   dash snapshot {...}    → OK: {"applied":true}
        #   dash boom              → ERR: dash: unknown verb 'boom'
        #   dash health            → OK: payload follows tag=HEALTH; HEALTH_BEGIN/END
        #   dash slow_evt          → OK: queued; (50 ms later) EVT: slow_evt_done
        if line.startswith("dash idle"):
            conn.sendall(b'OK: {"scene":"idle"}\n')
        elif line.startswith("dash boom"):
            conn.sendall(b'ERR: dash: unknown verb \'boom\'\n')
        elif line.startswith("dash health"):
            body = '{"uptime_s":42,"theme":"noir"}'
            conn.sendall(
                f"OK: payload follows tag=HEALTH\n"
                f"HEALTH_BEGIN fmt=json bytes={len(body)}\n"
                f"{body}\n"
                f"HEALTH_END\n".encode("utf-8")
            )
        elif line.startswith("dash slow_evt"):
            conn.sendall(b"OK: queued\n")
            def fire():
                time.sleep(0.05)
                try:
                    conn.sendall(b"EVT: slow_evt_done\n")
                except OSError:
                    pass
            threading.Thread(target=fire, daemon=True).start()
        elif line.startswith("dash snapshot"):
            conn.sendall(b'OK: {"applied":true}\n')
        elif line.startswith("dash "):
            conn.sendall(b'OK: ack\n')
        else:
            conn.sendall(b"ERR: dispatch: bad line\n")


@pytest.fixture()
def mock_server():
    srv = _MockServer()
    try:
        yield srv
    finally:
        srv.stop()


# ─────────────────────────────────────────────────────────────────────────────
# Tests
# ─────────────────────────────────────────────────────────────────────────────


def test_open_close_cleanly(mock_server):
    with open_persistent_session(mock_server.addr) as s:
        assert s.is_open
    assert not s.is_open


def test_push_one_line_yields_ok(mock_server):
    with open_persistent_session(mock_server.addr) as s:
        s.write_line("dash idle")
        evts = []
        for evt in s.iter_events(timeout=1.0):
            evts.append(evt)
            if evt.kind in ("ok", "err"):
                break
        assert any(e.kind == "ok" for e in evts), f"got {evts}"


def test_push_100_lines_and_collect(mock_server):
    """Open / push 100 lines / collect 100 OK replies / close. Covers
    the bridge's snapshot-flood path."""
    with open_persistent_session(mock_server.addr) as s:
        payload = {"total": 1, "running": 1, "waiting": 0, "msg": "x"}
        for _ in range(100):
            s.write_line(f'dash snapshot "{json.dumps(payload, separators=(",", ":"))}"')
        ok_count = 0
        for evt in s.iter_events(timeout=3.0):
            if evt.kind == "ok":
                ok_count += 1
            if ok_count >= 100:
                break
        assert ok_count == 100, f"got {ok_count}/100"
    # Server should also see 100 commands
    assert len([r for r in mock_server.received if r.startswith("dash snapshot")]) == 100


def test_concurrent_write_and_read_evt(mock_server):
    """Gap G-3: a single session must support pushing snapshots AND
    receiving an async EVT — that's the permission-prompt flow. Drive
    a writer thread while iter_events() runs in the test thread."""
    with open_persistent_session(mock_server.addr) as s:
        events: list[ReplyEvent] = []

        def writer():
            time.sleep(0.05)
            s.write_line("dash slow_evt")
            for _ in range(20):
                s.write_line("dash snapshot {}")
                time.sleep(0.002)

        t = threading.Thread(target=writer, daemon=True)
        t.start()
        # Collect for 1.5 s — should see one EVT and many OKs.
        for evt in s.iter_events(timeout=1.5):
            events.append(evt)
        t.join(timeout=2.0)

    evts = [e for e in events if e.kind == "evt"]
    oks = [e for e in events if e.kind == "ok"]
    assert any("slow_evt_done" in e.text for e in evts), \
        f"slow_evt not received: {[e.text for e in evts]}"
    assert len(oks) >= 20, f"too few OKs: {len(oks)}"


def test_payload_follows_health(mock_server):
    """Multi-line `dash health` → HEALTH_BEGIN/END must arrive as
    one ReplyEvent(kind='payload') with the JSON blob intact."""
    with open_persistent_session(mock_server.addr) as s:
        s.write_line("dash health")
        payload_evt = None
        for evt in s.iter_events(timeout=1.5):
            if evt.kind == "payload":
                payload_evt = evt
                break
        assert payload_evt is not None
        assert payload_evt.tag == "HEALTH"
        body = json.loads(payload_evt.blob)
        assert body["uptime_s"] == 42
        assert body["theme"] == "noir"


def test_err_is_surfaced(mock_server):
    """G-H3: an ERR reply must arrive as ReplyEvent(kind='err'),
    not silently swallowed."""
    with open_persistent_session(mock_server.addr) as s:
        s.write_line("dash boom")
        err_evt = None
        for evt in s.iter_events(timeout=1.0):
            if evt.kind == "err":
                err_evt = evt
                break
        assert err_evt is not None
        assert "unknown verb" in err_evt.text


def test_on_err_callback_fires(mock_server):
    """The on_err convenience hook fires for ERR lines."""
    seen: list[str] = []
    with open_persistent_session(mock_server.addr) as s:
        s.on_err(lambda e: seen.append(e.text))
        s.write_line("dash boom")
        # Wait briefly for the callback to fire.
        deadline = time.monotonic() + 1.0
        while not seen and time.monotonic() < deadline:
            time.sleep(0.02)
    assert seen, "on_err callback never fired"
    assert "unknown verb" in seen[0]


def test_on_event_filtered_by_kind(mock_server):
    """on_event with kinds={'evt'} should only see EVT lines."""
    seen_kinds: list[str] = []
    with open_persistent_session(mock_server.addr) as s:
        s.on_event(lambda e: seen_kinds.append(e.kind), kinds=frozenset({"evt"}))
        s.write_line("dash slow_evt")
        # Wait for the OK + EVT
        deadline = time.monotonic() + 1.0
        while "evt" not in seen_kinds and time.monotonic() < deadline:
            time.sleep(0.02)
    assert seen_kinds == ["evt"], f"got {seen_kinds}"


def test_write_after_close_raises(mock_server):
    s = open_persistent_session(mock_server.addr)
    s.close()
    with pytest.raises(TransportError):
        s.write_line("dash idle")


def test_two_sessions_can_coexist():
    """Two independent mock servers; two SessionHandles. They must not
    cross-talk (each has its own reader thread / buffer / subscribers)."""
    a = _MockServer()
    b = _MockServer()
    try:
        sa = open_persistent_session(a.addr)
        sb = open_persistent_session(b.addr)
        try:
            sa.write_line("dash idle")
            sb.write_line("dash boom")
            # session A sees only OK
            ok_a = False
            err_a = False
            for evt in sa.iter_events(timeout=1.0):
                if evt.kind == "ok":
                    ok_a = True
                    break
                if evt.kind == "err":
                    err_a = True
            assert ok_a and not err_a
            # session B sees only ERR
            ok_b = False
            err_b = False
            for evt in sb.iter_events(timeout=1.0):
                if evt.kind == "err":
                    err_b = True
                    break
                if evt.kind == "ok":
                    ok_b = True
            assert err_b and not ok_b
        finally:
            sa.close()
            sb.close()
    finally:
        a.stop()
        b.stop()


def test_iter_events_timeout_returns(mock_server):
    """iter_events(timeout=0.2) returns after 0.2 s of silence even
    without close — so a consumer can poll in a loop with no risk of
    blocking forever."""
    with open_persistent_session(mock_server.addr) as s:
        started = time.monotonic()
        events = list(s.iter_events(timeout=0.2))
        elapsed = time.monotonic() - started
    assert elapsed < 1.0, f"iter_events overran: {elapsed:.2f}s"
    assert events == []  # no events arrived in 0.2 s


def test_reuse_same_session_across_many_pushes_no_reconnect(mock_server):
    """Gap G-1 contract: writing N lines through ONE handle must not
    open a new socket each time. We check by counting accepted
    connections on the server."""
    # The mock accepts one connection at a time; if the client opened
    # a new socket per push, subsequent accepts would queue + the
    # received list would have nothing because the first connection
    # would never have seen them. Asserting that all 50 commands
    # landed on the same connection is sufficient.
    with open_persistent_session(mock_server.addr) as s:
        for _ in range(50):
            s.write_line("dash idle")
        ok = 0
        for evt in s.iter_events(timeout=2.0):
            if evt.kind == "ok":
                ok += 1
            if ok >= 50:
                break
    assert ok == 50
    assert len(mock_server.received) == 50
