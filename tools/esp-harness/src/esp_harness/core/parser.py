"""Python reference implementation of the firmware console tokeniser.

This is the canonical Python port of
``components/aurora-harness/src/console_protocol.c::dispatch_line`` so
that consumer projects (mocks, test rigs, host-side bridges) can parse
console lines exactly the way the device will, without copying the C
algorithm into every consumer.

The parity test in ``tests/test_parser.py`` pins the two implementations
together — when the C tokeniser changes (e.g. the G-7 fix at
``esp-harness@664b14e``), this file must be updated in the same commit
and the test corpus expanded to cover the new behaviour. Otherwise
mocks drift silently from the firmware (gap G-8 in the agent-dashboard
project).

Usage:

    from esp_harness.core.parser import tokenise_console_line, MAX_LINE
    argv = tokenise_console_line('dash snapshot "{\\"id\\":\\"req_1\\"}"')
    # → ['dash', 'snapshot', '{"id":"req_1"}']
"""

from __future__ import annotations

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
