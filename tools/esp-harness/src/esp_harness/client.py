"""Public client API for persistent device sessions (gaps G-1 + G-3).

The canonical way for a host-side bridge or test rig to talk to the
device over many commands is :func:`open_persistent_session` — it opens
ONE connection (serial or TCP), spawns a background reader thread that
parses every incoming line through a shared :class:`PayloadFollowsReader`,
and gives the caller a small handle with::

    write_line(line) -> None
    iter_events()    -> Iterator[ReplyEvent]   (live; blocks)
    drain_events()   -> list[ReplyEvent]       (non-blocking)
    on_event(cb)     -> registers a callback
    close()          -> None
    is_open

Why this API exists (gap G-1): each ``esp-harness console --cmd ...``
invocation pays ~140 ms of Python interpreter + import + port-open
overhead before the wire even sees the command. For high-rate bridges
(snapshot streams, EVT readers) that cost dominates. The persistent
session opens once and runs at wire speed.

Why this API exists (gap G-3): the EVT-reader loop and the snapshot
push loop in a bridge MUST share the open port — opening a second
connection to the same COM9 fails. Before this module the dashboard
bridge imported the private ``ConsoleSession`` and built its own reader
thread + state machine. That's now the documented, supported shape.

The serial transport delegates to :class:`ConsoleSession` to inherit
the DTR/RTS no-reset open and the line buffering. The TCP transport
goes straight to the socket and is intended for mocks
(``docs/mock_device.py`` in consumer projects).

ERR lines (gap G-H3) come through ``iter_events()`` as
``ReplyEvent(kind="err", ...)`` just like OK/EVT — consumers that
ignore them are making an explicit choice. A convenience ``on_err``
callback is also offered for the common "log + carry on" case.

The legacy ``esp-harness console --cmd ...`` and
``from esp_harness.core.console_session import ConsoleSession``
paths are NOT removed — this is a new layer ON TOP of the existing
machinery. Existing consumers keep working.
"""

from __future__ import annotations

import queue
import socket
import threading
import time
from dataclasses import dataclass
from typing import Callable, Iterator, Optional

from esp_harness.core.parser import PayloadFollowsReader, ReplyEvent

try:
    import serial  # type: ignore[import-untyped]
except ImportError:  # pragma: no cover — pyserial is a hard dep of the package
    serial = None  # type: ignore[assignment]


# ─────────────────────────────────────────────────────────────────────────────
# Transports
# ─────────────────────────────────────────────────────────────────────────────


class TransportError(Exception):
    """Raised on open/read/write failure. The handle marks itself closed."""


class _BaseTransport:
    """Minimal interface every transport must implement."""

    def open(self) -> None: ...
    def close(self) -> None: ...
    def is_open(self) -> bool: ...
    def write_line(self, line: str) -> None: ...
    def read_chunk(self, timeout_s: float) -> bytes:
        """Return up to N bytes within timeout_s. Empty bytes on idle/EOF."""
        ...


class _SerialTransport(_BaseTransport):
    """Persistent USB-Serial wrapped around pyserial directly.

    We don't reuse ConsoleSession for the byte loop because we need a
    raw read primitive that's safe to call from one thread while
    another calls write_line() on the same port. pyserial's read()/
    write() are both thread-safe; ConsoleSession's _iter_lines() owns
    a line buffer that isn't.
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
    ) -> None:
        if serial is None:
            raise TransportError("pyserial is required for serial transport")
        self.port_name = port
        self.baud = baud
        self.read_timeout = read_timeout
        self.write_timeout = write_timeout
        self.no_reset = no_reset
        self.settle_ms = settle_ms
        self._ser: Optional["serial.Serial"] = None  # type: ignore[name-defined]
        self._write_lock = threading.Lock()

    def open(self) -> None:
        if self._ser is not None:
            return
        ser = serial.Serial()
        ser.port = self.port_name
        ser.baudrate = self.baud
        ser.timeout = self.read_timeout
        ser.write_timeout = self.write_timeout
        if self.no_reset:
            ser.dtr = False
            ser.rts = False
        try:
            ser.open()
        except Exception as e:
            raise TransportError(f"serial open {self.port_name}: {e}") from e
        if self.no_reset:
            try:
                ser.dtr = False
                ser.rts = False
            except Exception:
                pass
        time.sleep(self.settle_ms / 1000.0)
        try:
            ser.reset_input_buffer()
        except Exception:
            pass
        self._ser = ser

    def close(self) -> None:
        if self._ser is not None:
            try:
                self._ser.close()
            except Exception:
                pass
            self._ser = None

    def is_open(self) -> bool:
        return self._ser is not None

    def write_line(self, line: str) -> None:
        if self._ser is None:
            raise TransportError("serial transport not open")
        data = (line.rstrip("\r\n") + "\n").encode("utf-8")
        with self._write_lock:
            try:
                self._ser.write(data)
                self._ser.flush()
            except Exception as e:
                raise TransportError(f"serial write: {e}") from e

    def read_chunk(self, timeout_s: float) -> bytes:
        if self._ser is None:
            raise TransportError("serial transport not open")
        # pyserial's read() returns whatever is available within its
        # configured `timeout`. We honour the caller's deadline by
        # bounding to the existing per-call timeout if the caller
        # wants longer; the reader loop calls this in a tight loop
        # anyway.
        try:
            return self._ser.read(4096) or b""
        except Exception as e:
            raise TransportError(f"serial read: {e}") from e


class _TCPTransport(_BaseTransport):
    """Newline-framed TCP. One persistent socket, thread-safe writes."""

    def __init__(self, addr: str, *, connect_timeout: float = 3.0) -> None:
        host, port_s = addr.split(":")
        self._host = host
        self._port = int(port_s)
        self._connect_timeout = connect_timeout
        self._sock: Optional[socket.socket] = None
        self._write_lock = threading.Lock()

    def open(self) -> None:
        if self._sock is not None:
            return
        try:
            sock = socket.create_connection(
                (self._host, self._port), timeout=self._connect_timeout,
            )
        except OSError as e:
            raise TransportError(
                f"tcp connect {self._host}:{self._port}: {e}"
            ) from e
        sock.settimeout(0.2)
        self._sock = sock

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def is_open(self) -> bool:
        return self._sock is not None

    def write_line(self, line: str) -> None:
        if self._sock is None:
            raise TransportError("tcp transport not open")
        data = (line.rstrip("\r\n") + "\n").encode("utf-8")
        with self._write_lock:
            try:
                self._sock.sendall(data)
            except OSError as e:
                raise TransportError(f"tcp write: {e}") from e

    def read_chunk(self, timeout_s: float) -> bytes:
        if self._sock is None:
            raise TransportError("tcp transport not open")
        # The socket has a short blocking timeout; we just call recv()
        # and let it raise socket.timeout if nothing's ready. The
        # reader loop calls this in a hot loop, so the per-call
        # timeout is enough.
        try:
            chunk = self._sock.recv(4096)
        except socket.timeout:
            return b""
        except OSError as e:
            raise TransportError(f"tcp read: {e}") from e
        if not chunk:
            raise TransportError("tcp peer closed connection")
        return chunk


def _make_transport(port: str, *, baud: int) -> _BaseTransport:
    """Pick the transport for *port*. ``host:port`` → TCP, else serial.

    The "looks like host:port" heuristic is: contains exactly one
    ``:`` and the part after is all digits. Bare COM9 / /dev/ttyUSB0
    take the serial path. (Windows ports are like "COM9" — never
    contain a colon — so this is unambiguous on the consumer side.)
    """
    if ":" in port:
        host, _, tail = port.partition(":")
        if tail.isdigit() and host:
            return _TCPTransport(port)
    return _SerialTransport(port, baud=baud)


# ─────────────────────────────────────────────────────────────────────────────
# SessionHandle
# ─────────────────────────────────────────────────────────────────────────────


@dataclass
class _Subscriber:
    cb: Callable[[ReplyEvent], None]
    kinds: Optional[frozenset[str]]  # None = all


class SessionHandle:
    """Live device session. One transport, one background reader thread.

    Write commands with :meth:`write_line`. Receive events through
    :meth:`iter_events` (blocking, generator) or by registering an
    :meth:`on_event` callback. Stop with :meth:`close` or use as a
    context manager.

    Concurrency contract: ``write_line()`` and ``iter_events()`` may
    be called from different threads. Multiple consumers may iterate
    simultaneously — each gets the events delivered after their
    iterator was first pulled (we use per-subscriber queues; older
    events are dropped if the subscriber falls behind).

    On transport error the handle marks itself closed and the reader
    thread exits. ``iter_events()`` will stop yielding; subsequent
    ``write_line()`` raises :class:`TransportError`.
    """

    def __init__(
        self,
        transport: _BaseTransport,
        *,
        max_queue: int = 4096,
    ) -> None:
        self._transport = transport
        self._reader = PayloadFollowsReader()
        self._buf = b""
        self._stop = threading.Event()
        self._closed = False
        self._lock = threading.RLock()
        self._subscribers: list[_Subscriber] = []
        self._max_queue = max_queue
        self._reader_thread: Optional[threading.Thread] = None
        # Default queue: every event the reader dispatches is also
        # pushed here. iter_events() / drain_events() consume from
        # this queue by default. Events buffered BEFORE a consumer
        # starts iterating are preserved (which is what bridges want:
        # write N lines, then drain).
        self._default_queue: queue.Queue[ReplyEvent] = queue.Queue(
            maxsize=max_queue,
        )
        # Extra subscriber queues (for multi-consumer cases). Each is
        # created by attach_queue(); they receive ONLY events that
        # arrive after attachment.
        self._extra_queues: list[queue.Queue[ReplyEvent]] = []

    # ── lifecycle ─────────────────────────────────────────────────
    def open(self) -> None:
        self._transport.open()
        self._stop.clear()
        self._closed = False
        self._reader_thread = threading.Thread(
            target=self._read_loop, name="esp-harness-reader", daemon=True,
        )
        self._reader_thread.start()

    def close(self) -> None:
        self._stop.set()
        self._closed = True
        try:
            self._transport.close()
        finally:
            # Wake any sleeping iter_events() consumers with a sentinel.
            with self._lock:
                extras = list(self._extra_queues)
                self._extra_queues.clear()
            for q in [self._default_queue, *extras]:
                try:
                    q.put_nowait(_SENTINEL)
                except queue.Full:
                    pass

    @property
    def is_open(self) -> bool:
        return not self._closed and self._transport.is_open()

    def __enter__(self) -> "SessionHandle":
        self.open()
        return self

    def __exit__(self, *_) -> None:
        self.close()

    # ── I/O ──────────────────────────────────────────────────────
    def write_line(self, line: str) -> None:
        """Send one line (newline appended). Thread-safe."""
        if self._closed:
            raise TransportError("session is closed")
        self._transport.write_line(line)

    def iter_events(
        self,
        *,
        timeout: Optional[float] = None,
        kinds: Optional[frozenset[str]] = None,
        queue_handle: Optional[queue.Queue[ReplyEvent]] = None,
    ) -> Iterator[ReplyEvent]:
        """Yield events as they arrive. Blocks until close or timeout.

        ``timeout`` (seconds) bounds the WAIT between events; once the
        timeout elapses with no event, the iterator returns. ``None``
        means block forever (until close).

        ``kinds`` filters to a subset of event kinds (e.g.
        ``{"evt", "err"}``). The default yields everything.

        ``queue_handle`` lets a caller bring a dedicated queue created
        via :meth:`attach_queue` so multiple iterators can run in
        parallel without contending for the default queue. Most
        callers leave it ``None`` and the default queue is used.

        Events buffered BEFORE this call started — between
        :meth:`open` and the first ``iter_events()`` invocation — ARE
        delivered (the default queue is filled by the reader from the
        moment the session opens). That's what high-rate bridges want:
        write 100 lines, drain.
        """
        q = queue_handle if queue_handle is not None else self._default_queue
        while True:
            try:
                evt = q.get(timeout=timeout if timeout is not None else 0.5)
            except queue.Empty:
                if timeout is not None:
                    return
                if self._closed:
                    return
                continue
            if evt is _SENTINEL:
                return
            if kinds is not None and evt.kind not in kinds:
                continue
            yield evt

    def drain_events(
        self,
        *,
        queue_handle: Optional[queue.Queue[ReplyEvent]] = None,
    ) -> list[ReplyEvent]:
        """Non-blocking: pop everything currently queued and return it."""
        q = queue_handle if queue_handle is not None else self._default_queue
        out: list[ReplyEvent] = []
        while True:
            try:
                evt = q.get_nowait()
            except queue.Empty:
                break
            if evt is _SENTINEL:
                break
            out.append(evt)
        return out

    def attach_queue(self, *, max_queue: Optional[int] = None) -> queue.Queue[ReplyEvent]:
        """Create and register an extra queue that receives all events
        from the moment of attachment. Pass it back through
        :meth:`iter_events`' ``queue_handle`` to consume from this
        queue independently of the default."""
        q: queue.Queue[ReplyEvent] = queue.Queue(
            maxsize=max_queue if max_queue is not None else self._max_queue,
        )
        with self._lock:
            self._extra_queues.append(q)
        return q

    def detach_queue(self, q: queue.Queue[ReplyEvent]) -> None:
        with self._lock:
            if q in self._extra_queues:
                self._extra_queues.remove(q)

    # ── callbacks ────────────────────────────────────────────────
    def on_event(
        self,
        cb: Callable[[ReplyEvent], None],
        *,
        kinds: Optional[frozenset[str]] = None,
    ) -> None:
        """Register a callback for every event whose kind matches."""
        with self._lock:
            self._subscribers.append(_Subscriber(cb=cb, kinds=kinds))

    def on_err(self, cb: Callable[[ReplyEvent], None]) -> None:
        """Convenience: register a callback for ERR lines (gap G-H3).

        Pre-v0.2 ``ConsoleSession`` swallowed ERR lines silently — the
        agent-dashboard bridge thought it had successfully pushed 10
        snapshots while the device side had errored on every single
        one. Surfacing ERRs is the default now; ``on_err`` makes the
        "log + carry on" case ergonomic.
        """
        self.on_event(cb, kinds=frozenset({"err"}))

    # ── reader thread ────────────────────────────────────────────
    def _read_loop(self) -> None:
        try:
            while not self._stop.is_set():
                try:
                    chunk = self._transport.read_chunk(0.5)
                except TransportError:
                    if not self._stop.is_set():
                        # Mark closed but let close() finish the
                        # subscriber drain.
                        self._closed = True
                    return
                if not chunk:
                    continue
                self._buf += chunk
                while b"\n" in self._buf:
                    one, self._buf = self._buf.split(b"\n", 1)
                    line = one.decode("utf-8", errors="replace").rstrip("\r")
                    for evt in self._reader.feed(line):
                        self._dispatch(evt)
        except Exception:
            # NEVER let the reader die unhandled — the consumer would
            # silently stop getting events.
            self._closed = True

    def _dispatch(self, evt: ReplyEvent) -> None:
        with self._lock:
            subs = list(self._subscribers)
            extras = list(self._extra_queues)
        for s in subs:
            if s.kinds is not None and evt.kind not in s.kinds:
                continue
            try:
                s.cb(evt)
            except Exception:
                # A subscriber raising must not break the reader.
                pass
        for q in (self._default_queue, *extras):
            try:
                q.put_nowait(evt)
            except queue.Full:
                # Slow consumer — drop oldest, retry. We do NOT block
                # the reader thread waiting for a slow consumer; that
                # would cascade into transport stall.
                try:
                    q.get_nowait()
                    q.put_nowait(evt)
                except (queue.Empty, queue.Full):
                    pass


# A sentinel value to wake iter_events() iterators on close.
_SENTINEL: ReplyEvent = ReplyEvent(kind="__sentinel__")


# ─────────────────────────────────────────────────────────────────────────────
# Public factory
# ─────────────────────────────────────────────────────────────────────────────


def open_persistent_session(
    port: str,
    *,
    baud: int = 115200,
    auto_open: bool = True,
) -> SessionHandle:
    """Open ONE long-lived session to the device.

    *port* is either a serial port name (``COM9``, ``/dev/ttyUSB0``) or
    a TCP endpoint (``127.0.0.1:9876``). The transport is selected
    automatically.

    The returned :class:`SessionHandle` may be used as a context
    manager, or closed explicitly with :meth:`SessionHandle.close`.
    A background reader thread is started which parses every incoming
    line through a :class:`PayloadFollowsReader` — multi-line replies
    arrive as one ``ReplyEvent(kind="payload", tag=..., blob=...)``
    event, single-line replies as ``ok``/``err``/``evt`` events.

    Example::

        from esp_harness.client import open_persistent_session

        with open_persistent_session("COM9") as s:
            s.on_err(lambda e: print("ERR:", e.text))
            for _ in range(100):
                s.write_line('dash snapshot "{...}"')
            # collect events for 2 s
            evts = []
            for evt in s.iter_events(timeout=2.0):
                evts.append(evt)
    """
    transport = _make_transport(port, baud=baud)
    handle = SessionHandle(transport)
    if auto_open:
        handle.open()
    return handle


# Re-export from one place for ergonomics.
__all__ = [
    "open_persistent_session",
    "SessionHandle",
    "TransportError",
    "ReplyEvent",
]
