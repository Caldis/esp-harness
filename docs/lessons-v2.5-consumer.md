# Lessons surfaced by the agent-dashboard v2.x cycle

The esp32-agent-dashboard project is the first major non-Aurora
consumer of esp-harness. Each cycle of building it has uncovered
patterns + anti-patterns worth lifting back upstream. This file is
the running record. Append; don't rewrite.

## v2.5 cycle (May 2026)

### L25-1 — Multi-column absolute label layout is an anti-pattern

**What happened.** scene_dashboard v2.5.0-rc tried a 5-column layout
(time / status / chip / verb / target) with `lv_obj_set_pos` per
column + `LV_LABEL_LONG_DOT`. When any column's text exceeded its
declared width, the rendered glyphs visually bled into the adjacent
column's slot — even though logical truncation happened correctly.
Result: row 4 showed "00:04 ok cc_a3 (login (42 hits)) Edit
src/auth.py" — `(login (42 hits))` (row 2's TARGET text) leaked
into row 4's VERB position.

**Why.** LVGL renders glyphs without strict label-bounding-box
clipping by default; `LONG_DOT` controls how truncation MARKERS are
inserted, not how glyphs are clipped at the right edge. With wide
glyphs, especially after grow-shrink of the layout during redraws,
remnant pixels persist outside the declared label width.

**Pattern.** Prefer **single-label-per-row** when the row contains
multiple semantic columns. Format the whole row into one string with
explicit `%.Ns` specifiers, render in one label with one font, set
the label's width to the full panel-inscribed area. Truncation is now
the LABEL's responsibility, not the column's.

**Where to apply this in esp-harness.** Aurora's scene_sessions
multi-card grid suffers from a similar (if less visible) variant.
Aurora's design escapes by keeping each "card" small + low text.
But the same crash class exists. Worth documenting in the
scene-framework guidance + the aurora-harness README.

### L25-2 — gcc format-truncation analyzer needs explicit %.Ns

**What happened.** A `snprintf(rest, sizeof(rest), "%s  %s  %s %s
%s", ts_str, status_glyph, fe->kind_short, fe->sid_short,
fe->text)` failed at compile time with
`-Werror=format-truncation`. The analyzer assumed each `%s` could
return up to 815 bytes, so it added them up against the buffer size.
Bumping the buffer to 256, 512, etc. doesn't help — the analyzer
sees raw `const char*` pointers as unbounded.

**Fix.** Add explicit max-width specifiers: `"%.6s  %.3s  %.3s %.6s
%.80s"`. gcc then knows each `%s` is capped and can compute a tight
upper bound.

**Where to apply this in esp-harness.** Add to the framework's C
style guide / sample code. Possibly add a smoke build with this flag
enabled to catch new cases.

### L25-3 — Bridge reconnect → DTR toggle → device reset → "unknown command" race

**What happened.** Each time `claude_buddy_bridge serve` opens
`COM9`, pyserial toggles DTR/RTS during port init. The Waveshare
ESP32-S3 board uses those as RESET + BOOT control, so opening the
port reboots the device. Bridge then immediately starts pushing
`dash snapshot` lines — but the device's `agent_commands_register()`
hasn't fired yet at boot, so it responds `ERR: unknown command:
dash`. After ~1 second the registration completes; subsequent pushes
succeed. The first 1-3 snapshots are silently lost.

**Why.** No handshake. The bridge has no signal that "device is
ready to accept dash commands".

**Pattern.** Bridge should send `?ping` first on connect, wait for
`OK: pong`, THEN start the snapshot stream. `?ping` is a builtin
that's registered at framework init, before consumer commands.

**Where to apply this in esp-harness.** The `ConsoleSession` API
should expose a `wait_ready()` helper that ping-handshakes before
returning. Better: `open_persistent_session(port, wait_ready=True)`
as the default.

### L25-4 — Snapshot wire-size cap requires graceful truncation across MULTIPLE field families

**What happened.** v2.4.0 added `awaiting_summary`, `awaiting_options`,
`awaiting_context` per agent. Combined with the existing entries[] +
cwd + msg, a multi-agent snapshot easily exceeded `CONSOLE_MAX_LINE
= 1024`. Without proper truncation, the device received `ERR: line
too long` and the user-visible state never updated.

**Pattern.** Bridge needs progressive belt-tightening: trim
entries[] oldest-first; drop awaiting_context from non-most-recent
waiting agents; drop cwd from non-waiting agents; drop oldest
non-waiting agents entirely. The most-recent waiting agent is
NEVER dropped (it's the takeover anchor).

**Where to apply this in esp-harness.** This is consumer-level
logic (per-protocol shape) but the pattern of "size-cap-aware
serialiser with priority hints" is general. Could be a
`framework/serialiser_with_budget.py` helper that takes
`(payload, max_bytes, priority_keys)` and trims keys in priority
order.

### L25-5 — "Tokens are infinite fuel" pattern: agent-emitted next-step options

**What happened.** v2.4.0's `<dash-state>` contract is the protocol-
level version of multi-agent UX. Every turn, Claude emits a
machine-readable block with `summary` + 2-4 `options` (short
executable phrases). The bridge extracts and forwards; the device
renders. User reads options, types a digit, agent gets that as
the next prompt verbatim.

**Pattern.** The contract is markdown-flavored, not JSON — fits
naturally in CC's existing transcript without escaping discipline,
and is easy to regex-extract. Future agents (Codex, Cursor, Aider)
can adopt the same convention.

**Where to apply this in esp-harness.** Could ship a
`harness/agent_contract.py` with the regex + extractor so other
consumer projects don't reinvent. The naming is concrete
(`<dash-state>`) — fine for a single-product contract, but if
esp-harness wants to host this as a generic, a more abstract tag
like `<agent-hint>` or `<next-options>` would be better.

### L25-6 — Vertical centering trumps fixed Y on round panels

**What happened.** scene_awaiting v2.4.0 had fixed Y positions for
glyph / headline / agent_chip / options. With 0 options or 1
option, content stuck to the top of the panel, leaving a void at
the bottom. User feedback: "your turn 的布局已经偏上了".

**Fix.** Compute total content height per-frame, distribute
whitespace evenly above/below the core group. tick() re-aligns each
element with the dynamic top offset.

**Where to apply this in esp-harness.** Could ship a
`harness/layout_helpers.h` with a `lv_obj_vcenter_group(objs,
n, y_min, y_max)` primitive. Round AMOLED panels are the framework's
target form factor — round-panel-aware layout helpers belong here.
