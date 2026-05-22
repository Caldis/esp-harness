# Lessons from v1.7 quality-convergence wave

What we shipped, what we found, and what process changes prevent the
class of bug we hit. Add to this file every release — it's the
"don't repeat the past" reference.

> **Audience**: future maintainers (including a returning-from-amnesia
> author). Each lesson is anchored to a real defect found during
> hardware verification of v1.7.0 on 2026-05-22.

---

## Lesson 1 — ESP-IDF APIs that return `esp_err_t` vs POSIX byte count

**What broke**: `audio tone` reported `bytes:0` despite the speaker
actually playing. `audio mic peak` reported `peak_dbfs:0.0` (full
scale) regardless of ambient noise. Two separate symptoms, one root
cause.

**Root cause**: `esp_codec_dev_write` and `esp_codec_dev_read` return
`esp_err_t` (0 = OK, negative = error). The implementation assumed
POSIX-style — that the return is the number of bytes processed. So
`total_bytes += written` accumulated zeros for every successful
write, and there was no error path because 0 doesn't satisfy
`written < 0`.

**Why we missed it**: the API name `_write` / `_read` looks POSIX,
the parameter order looks POSIX, the error path uses `< 0` (POSIX
convention). The IDF docs even have a comment in the data layer
saying "returns 0 on success" — but the device wrapper hides this.
We had a comment about it inside `audio_record_peak` ("NOT a byte
count — confusing but documented in code") but the **write** path
didn't carry the same warning.

**Rule**: any ESP-IDF function whose return type is `int` and not
explicitly documented as a byte count is `esp_err_t`. When wrapping
one, assume `esp_err_t` semantics until you've read the IDF source
for that function specifically. Common offenders:
`esp_codec_dev_*`, `i2c_master_*`, `spi_master_*`, `gptimer_*`.

**Process change**: any new wrapper around an IDF API gets a one-line
test that asserts a successful call **and** that the reported byte
count is non-zero. Pattern:
```python
def test_audio_tone_reports_real_bytes():
    r = console("audio tone 880 300 50")
    assert r["bytes"] > 0, f"likely esp_err_t/byte-count confusion: {r}"
```

---

## Lesson 2 — ADC throwaway after `esp_codec_dev_open`

**What broke**: every fresh `audio mic` / `audio loopback` reported
`peak_dbfs:0.0` (full-scale int16 sample). After the throwaway fix:
`peak_dbfs:-61.1` in a quiet room — three orders of magnitude
quieter than the false reading.

**Root cause**: the first DMA buffer after the I2S RX channel is
opened contains uninitialised / stale memory. That garbage rides to
INT16_MAX (or INT16_MIN — both produce full-scale dBFS) and pegs the
peak metric for the rest of the capture.

**Why we missed it**: we tested by ear that the loopback "worked" —
audio came back through the speaker. We never numerically verified
the `peak_dbfs` field, because "0 dBFS" looks like a number, not an
error.

**Rule**: any DMA-backed peripheral wants a throwaway read /
"warm-up" after `_open`. Codec, I2S, USB-host, ADC continuous mode —
all the same shape.

**Process change**: in `bench --quick`, include `audio mic 1000`
followed by an assertion `peak_dbfs > -90 AND peak_dbfs < -10`
(captures both "stuck-at-floor" and "stuck-at-clip"). Bench failure
gets caught before the user.

---

## Lesson 3 — `INT16_MIN` is not the negative of `INT16_MAX`

**What broke**: a single sample at -32768 (the legal minimum of
int16) gave `abs_v = 32768`, which then divides 32768/32767 = 1.00003
in float — log10 of that is just-positive — and `peak_dbfs` reads
+0.001 dBFS. Looks "stuck at 0" but is actually mathematically wrong
in a subtle way.

**Root cause**: the asymmetry of two's complement. `INT16_MIN = -32768`
but `INT16_MAX = +32767`. Naive `abs()` of `INT16_MIN` overflows.

**Fix**: cap `abs_v` at 32767 before dividing. Three places, one
pattern.

**Process change**: any code that takes the absolute value of a
signed-int audio / IMU / ADC sample saturates at the type's
positive maximum. Don't rely on `abs()` from `<stdlib.h>` — it
overflows silently on `INT_MIN`.

---

## Lesson 4 — User-intent flags in event handlers

**What broke**: `wifi disconnect` returned `OK: {"disconnected":true}`
but the device immediately re-associated to the same AP, because
the `WIFI_EVENT_STA_DISCONNECTED` handler runs an auto-reconnect
loop that doesn't distinguish "AP dropped us" from "user asked us
to disconnect."

**Fix**: a `volatile bool s_user_disconnect` flag set **before** the
`esp_wifi_disconnect()` call, checked by the event handler.

**Rule**: any "watchdog-style" auto-retry inside an event handler
needs an explicit suppression flag set by the action that initiated
the disconnect. The flag is set on the requesting task; the handler
runs on the event-loop task. Use `volatile` or atomics — never
trust task-local ordering between them.

**Process change**: every event-handler-driven auto-retry in this
codebase gets a `// user-intent flag: <name>` comment pointing at
the suppression mechanism. If there isn't one, file an issue.

---

## Lesson 5 — `sdkconfig.defaults` is consulted ONCE, not on every build

**What broke**: I edited `sdkconfig.defaults` to add
`CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=6`, ran `idf.py build`,
1.7 seconds later it said "build OK." But the value didn't take
effect — the sdkconfig was still cached from the previous build,
which had `=16`.

**Why we missed it**: a successful build with no diagnostic feels
like the change applied. There's no "settings changed, reconfig
needed" warning.

**Rule**: editing `sdkconfig.defaults` requires either:
- deleting `sdkconfig` and `build/` before the next build, OR
- running `idf.py reconfigure` first.

**Process change**: the toolkit's `build` command should detect
`sdkconfig.defaults` mtime > `sdkconfig` mtime and either auto-trigger
reconfigure or warn. (Tracked as Q9 in the v1.7.1 backlog.)

---

## Lesson 6 — kconfig `select` makes some settings unsettable

**What broke**: I tried to disable `CONFIG_ESP_WIFI_MBEDTLS_CRYPTO=n`
in `sdkconfig.defaults`. After clean rebuild,
`grep ESP_WIFI_MBEDTLS_CRYPTO sdkconfig` still showed `=y`.

**Root cause**: kconfig `select` makes a symbol *forced-on* by a
parent symbol. Once any component declares `select ESP_WIFI_MBEDTLS_CRYPTO`,
setting it `=n` in defaults is ignored — the selecting symbol must
be disabled instead.

**Rule**: when a `=n` doesn't stick, search the IDF source for
`select <SYMBOL>` to find what's forcing it. Often you fix it by
not pulling in the parent component (i.e. trim REQUIRES).

**Process change**: the v1.7 manifesto's "DRAM contention" troubleshooting
section now lists this trap with the grep command.

---

## Lesson 7 — Default WiFi TX buffer pool is huge

**What broke**: `esp_wifi_init` failed with `ESP_ERR_NO_MEM` even
though `int_free=39 KB`. The largest contiguous block was 28 KB but
WiFi wanted more — because `CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=16`
(default) reserves 16 × ~1700 B = ~27 KB by itself.

**Why it bit us**: Aurora is a display-heavy app where LVGL eats
most internal SRAM. Adding `esp_https_ota` REQUIRES pulled mbedtls
in, mbedtls pulled WIFI_MBEDTLS_CRYPTO=y, that bumped wifi's
contiguous DRAM requirement past our budget.

**Fix**: `STATIC_TX_BUFFER_NUM=6` for any project whose WiFi workload
is mostly RX (scan, OTA download, MQTT subscribe).

**Process change**: any project consuming aurora-harness should
inherit a `sdkconfig.defaults` snippet that pre-sizes WiFi for
"display + occasional RX" — not the gateway-router default. Track
this as `sim-base/sdkconfig.fragment.display-wifi`.

---

## Lesson 8 — `forget` and `disconnect` are NOT the same intent

**What broke**: Step 7 of the wifi verification — after `wifi forget`,
status reported `configured=false, connected=true, ssid="CC's Wi-Fi IoT"`.
Looks bizarre.

**Why it's actually fine**: `forget` clears NVS storage; `disconnect`
drops the link. They're orthogonal operations. But the JSON state
makes it look broken.

**Process change**: this is documented in the `wifi forget` help line:
`forget` clears creds, does NOT disconnect. Users who want both:
`wifi disconnect && wifi forget`. We chose NOT to bundle them
because "forget keeps me online for the current session" is a
legitimate use case (rotate the creds without dropping the running
TCP connections).

**Rule**: when two commands look like they should be one, document
WHY they're separate. Saves the next reader from "fixing" what
isn't broken.

---

## Lesson 9 — `tap`/`swipe` EVTs are fire-and-forget — RESOLVED in v1.7.1

**What broke** (in framework, not v1.7 specifically): we could not
verify from the host that `tap 233 233` actually landed on a widget.
The firmware emits `EVT: tap_hit x=... y=... obj=0x...` but it
arrives AFTER `OK: tap dispatched`, and the host's `console --cmd`
session had already closed.

**Fix** (v1.7.1 post-release follow-up): added `--wait-evt REGEX
--evt-timeout SECS` to `console --cmd`. The session stays open
until OK:/ERR: AND the named EVT (or timeout) arrives. The matched
EVT body lands in JSON output's `matched_evt` field. Smoke gate
`tap --wait-evt captures tap_hit (L9 regression)` locks it in.

**Usage**:
```
esp-harness console --cmd "tap 233 233" --wait-evt "^tap_hit" --evt-timeout 2
esp-harness console --cmd "?ota download url=..." --wait-evt "OTA progress=100" --evt-timeout 60
```

**Rule**: every async command that emits progress / completion via
EVT should be exercised with `--wait-evt` at least once in smoke
testing. ERR short-circuits the EVT wait (no point sitting on an
already-failed command).

---

## Lesson 10 — `.env` should exist from day one of any test phase

**What broke**: the user provided their WiFi credentials inline in
chat. Credentials live in chat history and conversation summaries.
Suboptimal.

**Fix**: `.env` file at repo root (gitignored), `TEST_WIFI_SSID=`
and `TEST_WIFI_PASS=`. Any test script reads from there.

**Process change**: when starting any test phase that needs secrets,
the FIRST step is "create .env and gitignore it." Don't accept
inline secrets in chat.

---

## Lesson 11 — `X and (list or list)` returns the operand, not a bool

**What broke** (round-2 adversarial subagent finding): `manifest --port
COM9 --json` returned `device.available` as an 18-element command list
instead of `true`. AI agents branching on `if mfst["device"]["available"]`
silently got a truthy list — looked fine until something downstream
indexed into a "bool" and crashed in the agent.

**Root cause**: `dm.fetched_ok = dm.fetch_error is None and (dm.commands
or dm.scenes)`. Python's `and` returns the operand, not a bool, so when
both sides are truthy the result is the last operand (the list itself).

**Fix**: wrap the whole expression with `bool(...)`. One character, but
the failure mode was completely silent until the subagent checked the
type of the field.

**Process change**: any field that goes into a JSON contract document
(manifest, doctor, build result, etc.) must be type-coerced explicitly:
`bool(...)`, `int(...)`, `str(...)`. Truthiness is fine inside Python;
JSON contracts need real types.

**Smoke gate**: `manifest.device.available is a real bool (R2-bug
regression)` — runs against a connected device, asserts `isinstance bool`.

---

## Lesson 12 — Async EVT can arrive BEFORE the synchronous ack

**What broke** (round-2): the v1.7.1 `--wait-evt REGEX` feature
correctly captured `tap_hit` (which is emitted by an `lv_async_call`
that runs after the OK: reply), but missed `scene_changed` (which is
emitted synchronously *during* the `cmd_scene` handler, before
`console_reply_ok`). The original implementation gated EVT-regex
matching on `ack_seen`, so EVTs that arrived in the bytes before OK:
landed in `resp.events` but never tested.

**Why we missed it**: the L9 fix was driven by `tap` — for which the
async/sync timing happens to be "EVT after ack." Generalising to "any
async progress emission" introduced a hidden assumption.

**Fix**: every EVT runs through the regex regardless of ack state. If a
match arrives pre-ack, save it but keep reading so `resp.ok` ends up
correctly populated from the OK: line. If post-ack, the existing
early-break logic applies.

**Rule**: ack/EVT ordering is firmware-implementation-dependent. Host
parsers must never assume "EVT can only arrive after OK" — that's a
correctness violation, not an optimisation.

**Smoke gate**: `scene next --wait-evt captures pre-ack EVT (R2-bug
regression)`.

---

## Lesson 13 — Scaffold should tell users the truth about what works

**What broke** (round-2): `esp-harness new my-thing --component-source
vendor` printed `cd my-thing && esp-harness build` as the next-step
hint — but vendored mode doesn't include a BSP, so the build was
guaranteed to fail. Similarly `--component-source depend` printed
"build" even though the registry doesn't host aurora-harness yet.

**Root cause**: the success-message template was written once for the
default mode and reused. Each mode has a different ready state.

**Fix**: per-mode `next_steps` text. `link` says "cd / build / flash"
(works as-is). `vendor` says "before build, copy the BSP into
components/" with the exact path. `depend` says "registry-not-published,
use link or vendor instead".

**Rule**: success messages must be honest about what the user can
immediately do next. If the next step will fail, say so before they
try it. "Build" is not a generic placeholder.

**Process change**: any scaffolder / template emitter that prints a
"next steps" block must enumerate the modes it supports and gate each
on whether the resulting project is actually buildable.

---

## Lesson 14 — Version literals always drift; pin to source

**What broke** (round-2): three places held the literal `1.5.0` while
the project had moved to `1.7.1`: root `README.md` "Current release"
line, `tools/esp-harness/README.md` version badge, and the scaffolded
project's `idf_component.yml` aurora-harness pin (`^1.5.0`).

**Root cause**: human discipline. Each release would need someone to
remember to bump three+ unrelated files.

**Fix**: the scaffolder pin now reads `__version__` at scaffold time
via `_pinnable_version()`. The READMEs were bumped to 1.7.1; the
smoke gate now asserts `--version != "1.5.0"` so a future drift
trips the gate.

**Rule**: never hand-maintain a version literal in source. Either
substitute at build/install time, or assert in CI that it tracks.

---

## Convergence summary (v1.7.0 → v1.7.1)

| Defect | Lesson | Severity | Detected by |
|---|---|---|---|
| `audio tone bytes:0` | L1 | bug — wrong JSON | hardware E2E |
| `audio peak_dbfs:0.0` | L2 + L3 | bug — wrong JSON | hardware E2E |
| `wifi_init ENOMEM` | L5 + L7 | bug — feature unusable | hardware E2E |
| `disconnect auto-retry` | L4 | bug — wrong behaviour | hardware E2E |
| Console tokenizer no quotes | — | bug — feature limit | hardware E2E |
| `MBEDTLS_CRYPTO=n` ignored | L6 | trap — investigation cost | hardware E2E |
| `tap_hit` EVT unobservable | L9 | framework gap | tap regression |
| `imu.accel` semantics misleading | — | doc gap | sensor regression |

8 defects from one verification pass. All would have been caught by
a `tools/smoke.ps1` pre-release script that runs every command and
checks its JSON contract — that's the v1.7.1 / Q9 deliverable.

---

## Quality-control gates going forward

1. **Pre-commit**: nothing, deliberately. Devs need fast iteration.
2. **Pre-release**: `tools/smoke.ps1` (Q9) runs:
   - `doctor` 8/8
   - `pytest` 3/3
   - `sim diff` 13/13 identical
   - device 5-step roundtrip (ping → stat → sensor → power → sys)
   - audio tone (bytes > 0), audio mic (peak between [-90, -10])
   - wifi scan (count ≥ 1)
   - `?ota info` (running label matches expected slot)
3. **Post-release**: this file. Add a section for every release with
   what we found, in the same format.

The bar isn't "every test passes." It's "every test that has ever
caught a real bug runs every release."
