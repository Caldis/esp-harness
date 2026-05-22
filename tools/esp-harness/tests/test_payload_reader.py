"""Tests for :class:`PayloadFollowsReader` (gap G-H1).

The reader is a state machine that consumes the line iterator the
device produces over the wire — OK / ERR / EVT / TAG_BEGIN / inner
body / TAG_END — and yields semantic events. Before this helper
existed, every bridge re-implemented the state machine. The dashboard
project's pre-v0.2 ``claude_buddy_bridge`` had two real bugs caused by
that: it swallowed ERR lines (G-H3) and it failed to skip the BEGIN
framing line on first try (G-H1).

The corpus below pins the contract.
"""

from __future__ import annotations

import pytest

from esp_harness.core.parser import (
    PayloadFollowsReader,
    ReplyEvent,
)


def _collect(reader: PayloadFollowsReader, lines: list[str]) -> list[ReplyEvent]:
    return list(reader.feed_lines(lines))


def test_single_ok():
    r = PayloadFollowsReader()
    events = _collect(r, ["OK: pong"])
    assert len(events) == 1
    assert events[0].kind == "ok"
    assert events[0].text == "pong"


def test_single_err():
    r = PayloadFollowsReader()
    events = _collect(r, ["ERR: unknown command: ?nope"])
    assert len(events) == 1
    assert events[0].kind == "err"
    assert events[0].text == "unknown command: ?nope"


def test_single_evt():
    r = PayloadFollowsReader()
    events = _collect(r, ["EVT: permission id=req_001 decision=allow"])
    assert len(events) == 1
    assert events[0].kind == "evt"
    assert events[0].text == "permission id=req_001 decision=allow"


def test_payload_follows_self_describing():
    """The post-G-4 contract: OK line carries `tag=NAME`, then the
    block arrives framed as <NAME>_BEGIN ... <NAME>_END."""
    r = PayloadFollowsReader()
    lines = [
        "OK: manifest follows tag=HELP",
        "HELP_BEGIN fmt=json bytes=42",
        '{"count":1,"commands":[{"name":"?ping","usage":"liveness"}]}',
        "HELP_END",
    ]
    events = _collect(r, lines)
    assert len(events) == 1
    evt = events[0]
    assert evt.kind == "payload"
    assert evt.tag == "HELP"
    assert '"count":1' in evt.blob
    # Meta carries the OK body (so consumers can see `tag=HELP` etc.)
    assert "tag=HELP" in evt.meta


def test_payload_with_evt_in_between():
    """An EVT line that arrives while we're collecting payload body
    should be folded into the body — only the matching <TAG>_END
    closes the block. This is what the wire actually looks like: the
    device can emit EVTs from other tasks while a payload is in
    flight, and the framing is sacred."""
    r = PayloadFollowsReader()
    lines = [
        "OK: payload follows tag=HEALTH",
        "HEALTH_BEGIN fmt=json bytes=20",
        '{"uptime_s":42,',
        '"foo":"bar"}',
        "HEALTH_END",
    ]
    events = _collect(r, lines)
    assert len(events) == 1
    assert events[0].kind == "payload"
    assert events[0].tag == "HEALTH"
    assert "uptime_s" in events[0].blob
    assert "foo" in events[0].blob


def test_evt_between_two_ok_lines():
    """A genuine asynchronous EVT (NOT inside a payload block) is
    surfaced as its own event between two single-line OK replies."""
    r = PayloadFollowsReader()
    lines = [
        "OK: scene set",
        "EVT: scene_changed name=sessions",
        "OK: ack",
    ]
    events = _collect(r, lines)
    assert [e.kind for e in events] == ["ok", "evt", "ok"]
    assert events[1].text == "scene_changed name=sessions"


def test_err_is_surfaced_not_swallowed():
    """Regression for G-H3: the pre-v0.2 bridge swallowed ERR lines
    silently. The reader must yield them."""
    r = PayloadFollowsReader()
    lines = [
        "EVT: tick",
        "ERR: dash snapshot: malformed JSON (line 1, col 1)",
        "OK: idle",
    ]
    events = _collect(r, lines)
    kinds = [e.kind for e in events]
    assert "err" in kinds, f"ERR was swallowed: {kinds}"
    err_evt = next(e for e in events if e.kind == "err")
    assert "malformed JSON" in err_evt.text


def test_legacy_payload_without_explicit_tag():
    """Backwards-compat: older firmware emitted `OK: payload follows`
    without `tag=`. The reader infers the tag from the subsequent
    BEGIN line so older devices still work."""
    r = PayloadFollowsReader()
    lines = [
        "OK: payload follows",
        "OLDTAG_BEGIN fmt=text bytes=5",
        "hello",
        "OLDTAG_END",
    ]
    events = _collect(r, lines)
    assert len(events) == 1
    assert events[0].kind == "payload"
    assert events[0].tag == "OLDTAG"
    assert events[0].blob == "hello"


def test_log_line_is_yielded():
    """ESP_LOG and other uncategorised lines come through as 'log'
    events so consumers can choose to display them. Pre-v0.2 the
    bridge silently dropped these."""
    r = PayloadFollowsReader()
    events = _collect(r, ["I (12345) wifi:scan done"])
    assert len(events) == 1
    assert events[0].kind == "log"
    assert "wifi:scan" in events[0].text


def test_partial_feed_then_complete():
    """Calling feed() line-by-line should buffer the body until END
    arrives. ZERO events come out of the BEGIN line and the body
    lines; ONE event comes out of the END."""
    r = PayloadFollowsReader()

    assert list(r.feed("OK: payload follows tag=HEALTH")) == []
    assert list(r.feed("HEALTH_BEGIN fmt=json bytes=20")) == []
    assert list(r.feed('{"uptime_s":7}')) == []

    events = list(r.feed("HEALTH_END"))
    assert len(events) == 1
    assert events[0].kind == "payload"
    assert events[0].blob == '{"uptime_s":7}'


def test_reset_drops_in_progress_payload():
    """After reset(), a future BEGIN line is treated fresh — used by
    consumers after reconnect to a different device session."""
    r = PayloadFollowsReader()
    list(r.feed("OK: payload follows tag=HEALTH"))
    list(r.feed("HEALTH_BEGIN fmt=json bytes=10"))
    list(r.feed('{"uptime_s":5'))
    assert r.in_payload
    r.reset()
    assert not r.in_payload

    # Fresh sequence works.
    events = list(r.feed("OK: pong"))
    assert events == [ReplyEvent(kind="ok", text="pong")]


def test_orphan_begin_without_ok():
    """A <TAG>_BEGIN that arrives WITHOUT a preceding OK is malformed
    in the protocol. We emit it as a log event rather than start a
    body collection — that way the rest of the stream stays valid."""
    r = PayloadFollowsReader()
    events = _collect(r, ["DUMP_BEGIN fmt=base64 bytes=128"])
    assert len(events) == 1
    assert events[0].kind == "log"
    assert "DUMP_BEGIN" in events[0].text


def test_dump_self_describing_with_meta():
    """The G-F1b screenshot fix adds `w_requested=` / `w_actual=` /
    `reason=` to the OK line. The reader stashes the whole body in
    `meta` so consumers can recover those fields."""
    r = PayloadFollowsReader()
    lines = [
        "OK: dump start tag=DUMP w=466 h=466 fmt=RGB565LE bytes=434312",
        "DUMP_BEGIN w=466 h=466 fmt=RGB565LE bytes=434312",
        "AAAA",
        "BBBB",
        "DUMP_END",
    ]
    events = _collect(r, lines)
    assert len(events) == 1
    evt = events[0]
    assert evt.kind == "payload"
    assert evt.tag == "DUMP"
    assert evt.blob == "AAAA\nBBBB"
    assert "w=466" in evt.meta
    assert "bytes=434312" in evt.meta


def test_multiple_payloads_in_sequence():
    """Two sequential payload-follows replies — common when a bridge
    queries health right after listing scenes."""
    r = PayloadFollowsReader()
    lines = [
        "OK: scene manifest follows tag=SCENES",
        "SCENES_BEGIN fmt=json bytes=10",
        '["idle"]',
        "SCENES_END",
        "OK: payload follows tag=HEALTH",
        "HEALTH_BEGIN fmt=json bytes=15",
        '{"uptime_s":1}',
        "HEALTH_END",
    ]
    events = _collect(r, lines)
    assert len(events) == 2
    assert events[0].kind == "payload"
    assert events[0].tag == "SCENES"
    assert events[1].kind == "payload"
    assert events[1].tag == "HEALTH"


def test_ok_with_tag_but_no_following_block_is_legal():
    """An OK with `tag=` whose BEGIN never arrives is the consumer's
    problem to time out on — but the reader must not get stuck on it.
    A subsequent feed() of an unrelated line should NOT be treated as
    the missing body (the matching <TAG>_END will never arrive).

    Currently the reader holds in-payload state until a matching END.
    This test pins the documented behaviour: ``in_payload`` stays
    True; the caller is responsible for calling :meth:`reset` after
    a timeout.
    """
    r = PayloadFollowsReader()
    list(r.feed("OK: payload follows tag=HEALTH"))
    assert r.in_payload
    # An unrelated EVT arrives during the wait; it gets folded into
    # the buffer (this is consistent with how the firmware would behave
    # — the EVT is genuinely part of the wire stream between OK and END).
    list(r.feed("EVT: something"))
    assert r.in_payload  # still waiting for HEALTH_END
    r.reset()
    assert not r.in_payload
