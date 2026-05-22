"""Parity tests for the Python tokeniser against the C firmware.

The C implementation lives at
``components/aurora-harness/src/console_protocol.c::dispatch_line``.
This test file pins the Python port at
``src/esp_harness/core/parser.py`` to that contract by enumerating
the cases that matter on the wire — empty lines, plain argv, quoted
spaces, the post-G-7 nested-JSON case, the MAX_ARGS cap, etc.

When the C tokeniser changes, this corpus must grow with it. The
``agent-dashboard@G-8`` gap is the motivation: consumer mocks that
copy the algorithm drift silently the moment the firmware moves.
"""

from __future__ import annotations

import pytest

from esp_harness.core.parser import MAX_ARGS, tokenise_console_line


@pytest.mark.parametrize(
    "line, expected",
    [
        # ── basics ─────────────────────────────────────────────────
        ("",                                  []),
        ("    ",                              []),
        # The firmware's byte-loop strips `\r` and uses `\n` as the
        # line terminator, so dispatch_line never sees `\n` and only
        # sees a *trailing* `\r` (trim handles it). We test bare `\r`
        # tails — embedded `\n` is unreachable on the real wire.
        ("\r",                                []),
        ("ping",                              ["ping"]),
        ("?ping",                             ["?ping"]),
        ("dash idle",                         ["dash", "idle"]),
        ("  dash   idle  ",                   ["dash", "idle"]),
        ("dash idle\r",                       ["dash", "idle"]),
        ("dash idle\t\r",                     ["dash", "idle"]),
        ("a b c d",                           ["a", "b", "c", "d"]),

        # ── unquoted-token legacy quoting (wifi-style) ────────────
        # `ssid="My Wi-Fi"` — quotes toggle in_quote, then are stripped.
        ('wifi connect ssid="My Wi-Fi"',
         ["wifi", "connect", "ssid=My Wi-Fi"]),
        ('wifi connect ssid="A" pass="B"',
         ["wifi", "connect", "ssid=A", "pass=B"]),

        # ── quote-leading tokens (post-G-7 nested-JSON contract) ──
        # Inner `"` must survive.
        ('dash prompt "{\\"id\\":\\"req_1\\"}"',
         ["dash", "prompt", '{\\"id\\":\\"req_1\\"}']),
        ('dash snapshot "{\\"total\\":2,\\"running\\":1}"',
         ["dash", "snapshot", '{\\"total\\":2,\\"running\\":1}']),
        # The form `dash prompt "{"id":"req"}"` (no backslashes) — the
        # outer-quote pair survives because none of the inner `"` are
        # followed by whitespace or EOL. We end up with the verbatim
        # nested-JSON string.
        ('dash prompt "{"id":"req_1","tool":"Bash"}"',
         ["dash", "prompt", '{"id":"req_1","tool":"Bash"}']),
        # Quote-leading token with no inner quotes — closes normally.
        ('dash msg "hello world"',
         ["dash", "msg", "hello world"]),
        # Quote-leading token next to another arg.
        ('cmd "a b c" tail',
         ["cmd", "a b c", "tail"]),

        # ── empty quoted token ────────────────────────────────────
        ('cmd ""',                            ["cmd", ""]),

        # ── trailing whitespace inside / outside quotes ───────────
        ('dash idle    ',                     ["dash", "idle"]),
        ('"leading-quoted-token"',            ["leading-quoted-token"]),
    ],
)
def test_known_cases(line: str, expected: list[str]) -> None:
    assert tokenise_console_line(line) == expected


def test_max_args_cap() -> None:
    """Anything past MAX_ARGS tokens is silently dropped (firmware
    parity)."""
    # 12 plain tokens — should be capped at MAX_ARGS.
    line = " ".join(f"t{i}" for i in range(12))
    out = tokenise_console_line(line)
    assert len(out) == MAX_ARGS
    assert out == [f"t{i}" for i in range(MAX_ARGS)]


def test_g7_regression_payload_round_trip() -> None:
    """The specific shape the agent-dashboard bridge sends. If this
    regresses, G-7 has come back."""
    payload = '{"id":"req_001","tool":"Bash","hint":"rm -rf /tmp/foo"}'
    line = f'dash prompt "{payload}"'
    argv = tokenise_console_line(line)
    assert argv[0] == "dash"
    assert argv[1] == "prompt"
    # Inner quotes must be intact so cJSON / tiny_json can parse it.
    assert argv[2] == payload


def test_quote_leading_inner_quote_followed_by_text_is_literal() -> None:
    """`"a"b` inside a quote-leading token: the `"` is NOT a closing
    quote because what follows isn't whitespace/EOL. It's a literal
    embedded quote."""
    line = '"a"b c"d"'
    # Single quote-leading token: opens at index 0, sees `a`, sees `"`
    # followed by `b` (non-ws), so `"` is literal; then `b`; then ` `
    # — but we're in_quote=True, so space is preserved? No: the
    # quote-leading branch breaks on the matching close-quote at EOT.
    # Once the `"` at position 2 is treated as embedded (kept), the
    # token has not closed yet, so the next space is INSIDE the quote
    # and gets eaten by the in_quote check (which only breaks on space
    # when not-in-quote). The whole rest of the line is one token.
    # End-of-line reached without a closing `"` followed by ws/EOL,
    # so the token just terminates at end-of-line.
    out = tokenise_console_line(line)
    assert out == ['a"b c"d']


def test_quoted_token_unterminated() -> None:
    """An unterminated quote-leading token: everything after the
    opening `"` is one token, no error (firmware also accepts this —
    it just runs the line off the end). The C version would NUL-
    terminate after the last byte; we just stop."""
    out = tokenise_console_line('cmd "abc')
    assert out == ["cmd", "abc"]


def test_no_args_returns_empty() -> None:
    """`""` (just whitespace, just CR) — never produces a token."""
    assert tokenise_console_line("\r") == []
    assert tokenise_console_line("\r ") == []
    assert tokenise_console_line(" \t \t ") == []
