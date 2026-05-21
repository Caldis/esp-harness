# Manifesto

> The "why" behind every design decision in esp-harness, written so the
> rationale survives when none of the original contributors do.

## The problem we exist to solve

Embedded firmware development has a feedback-loop problem.

The default loop for an ESP-IDF + LVGL project is: edit `.c` →
`idf.py build` (30 s) → `idf.py flash` (25 s) → `idf.py monitor` →
*scroll through unstructured serial log looking for evidence the change
worked* → 30 s later you're back at the editor. Five-minute iterations.
Most of the time is waiting + parsing.

Worse, the device's side of the conversation is **unstructured text**.
A scene-changed event is `I (12345) aurora: scene_changed idx=3` —
which means an AI agent (or a CI script, or a regression test) has to
either grep that exact format or hard-code parsing logic. Every project
re-invents this from scratch.

esp-harness exists to <b>shorten that loop and make every device output
parseable by something other than a human</b>.

## What we believe

### 1. Discovery is a single command, not a documentation problem.

`esp-harness manifest --json` returns a JSON document enumerating
every toolkit command, every device console command, every registered
scene. If a capability isn't in the manifest, **it doesn't exist for the
AI**. That's a stronger constraint than "you should document things",
because it's enforced by code at registration time.

### 2. The host build is part of the dev loop, not an optional bonus.

The host LVGL simulator (`sim-base/` + per-example sim) is the
difference between 30-second flash cycles and 5-second host cycles.
For pure-UI iteration, 6× speed-up matters more than fidelity to the
device's exact pixel-level rendering. The sim is intentionally
*incomplete* (no real peripherals, no real timing) because completeness
would slow it down to roughly device speed and defeat its purpose.

### 3. Every device output is OK / ERR / EVT.

Three line prefixes, never more, never less. Everything else fits in
payload frames. A parser that gets these three right doesn't care what
protocol version or firmware version it's talking to.

### 4. The library is small. The application owns the application.

`aurora-harness` contains exactly what every LVGL + console project
re-invents: console protocol, scene framework, toast/progress
overlays, default `?stat` / `scene` / `tap` / `swipe` / `?dump`. It
does **not** contain peripheral drivers, application-specific
commands, business logic, theming, or any kind of "smart" default UI
chrome. Those belong in the application's own code. Resist the
temptation to add app-level convenience to the component — it taxes
every future consumer.

### 5. Each commit verifies on its own.

There's no "fix it in the next commit" allowed. `esp-harness test`
runs in 10 seconds and catches manifest drift, sim regressions, and
doctor health. If the loop's broken, that's the only signal needed.

### 6. Friction lives at the seams.

We accept ugly path-resolution code, ugly platform-detection scripts,
ugly SDL2 link-order workarounds — anywhere the user doesn't see them.
We don't accept ugly APIs the user does see. Beauty budget goes where
it's read most.

### 7. Examples are first-class.

`examples/<name>/` directories are not afterthoughts. They're the
proof that the library is real, the reference for "how to use this",
and the regression detectors via sim diff. A library without a
worked example is a draft RFC, not a release.

## What we don't do

The decisions below are <b>intentional out-of-scope</b>. They keep coming
up; the answer keeps being "not in scope".

| We will not | Why |
|---|---|
| **Embed peripheral drivers in aurora-harness** | Peripherals are board-specific and application-specific. Every project's IMU/PMIC/audio chain is different. The component would bloat and become hard to consume. |
| **Replace ESP-IDF or PlatformIO** | We run *alongside* ESP-IDF, calling `idf.py` under the hood. We are not a competing build system. PlatformIO compatibility is a future "nice to have", not a goal. |
| **Add a GUI / TUI** | The CLI + JSON contract is the AI-driveable interface. A TUI would optimise for the wrong user. Users who want a GUI use VSCode's ESP-IDF extension. |
| **Keep deprecated APIs across major versions** | Semver is honest. v2.0 will rename or remove things. We don't carry around backwards-compatibility shims for forever; legacy users pin a major version. |
| **Auto-update vendored components** | When you `esp-harness new --component-source vendor`, the copy is yours. You decide when to re-vendor. We won't ship a magic auto-update path that surprises users. |
| **Pretend to support boards we haven't tested** | `boards/` contains exactly the BSPs we've actually run firmware on. Adding a "C6 minimal" board means actually flashing C6 hardware, not just declaring it. |
| **Solve "AI does software engineering"** | We solve the specific problem of "AI drives an ESP32 dev loop". The broader question of AI software engineering is somebody else's product. |
| **Multi-device orchestration in a single command** | Fleet management is a v2.0+ conversation that requires a different architecture (probably a daemon + RPC). Today, one device per `esp-harness` invocation. |

## What "done" looks like for each layer

### `aurora-harness` is done when

- Any ESP-IDF + LVGL project can vendor it in one CMake line.
- Every public header has one-page docs in the README.
- Porting to a non-Espressif board needs ≤ 1 file (the BSP shim) and is documented in `PORTING.md`.
- The component is published on the ESP-IDF Component Registry.

### `tools/esp-harness` is done when

- `pip install esp-harness` works from PyPI.
- Every command has a structured JSON output, a meaningful exit code, and a one-line `--help`.
- `esp-harness doctor` covers every dep needed by any command.
- `esp-harness new` produces a project that builds + flashes + boots without further intervention.

### The sim is done when

- Any peripheral-free scene from any example runs on the host.
- Visual regression catches a 1-pixel intentional change reliably.
- The host build takes ≤ 5 seconds incremental.

### The docs are done when

- A new contributor can land their first PR within an hour.
- A user with no prior context can run the Aurora demo in 10 minutes.
- A user can scaffold their own project and have a custom scene rendering in 30 minutes.
- An AI session reading only `manifest --json` + `AGENT.md` can be productive on day one.

## What we measure

Concrete signals that we're succeeding:

| Signal | Target |
|---|---|
| Time from `git clone` to "Aurora demo flashed and running" | ≤ 10 min |
| Time from `esp-harness new my-thing` to "my custom scene visible on device" | ≤ 30 min |
| Lines of code in a new project's `main/` after using `new --component-source vendor` | < 100 |
| Failure modes covered by `esp-harness doctor` hints | ≥ 90% of fresh-clone install issues |
| `esp-harness test` runtime | ≤ 15 s |

These are guideposts, not contracts. If a target stops being useful, it changes.

## How we make decisions

When something is ambiguous (a feature request, an architectural choice,
a "should we do PIO" discussion):

1. **Does it shorten the dev loop?** If no, hard to justify.
2. **Does it preserve the discovery surface?** If it adds a capability
   not registrable via `manifest`, that's a red flag.
3. **Does it survive the amnesia test?** If a fresh reader of the docs
   can't see why the choice was made, the choice probably isn't worth
   it.
4. **Is there a smaller version that captures 80% of the value?** If
   yes, do that first.

That's the whole decision framework.

## Predecessor history

The project evolved from two earlier repos (`esp32-harness-showcase`
v1.0–v1.4 and `esp32-harness-toolkit` v1.0) which are kept as
archives. The lessons of those iterations are baked into the
conventions above. Both repos' `CHANGELOG.md` retain the longer
day-by-day history; this manifesto is the distilled "why".
