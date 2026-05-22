# Changelog

Repo-level milestones. Per-artifact changelogs live in:

- [`components/aurora-harness/`](./components/aurora-harness/) (component history; preserved from `esp32-harness-showcase` v1.0-v1.4)
- [`tools/esp-harness/CHANGELOG.md`](./tools/esp-harness/CHANGELOG.md) (toolkit history; preserved from `esp32-harness-toolkit`)
- [`examples/aurora/CHANGELOG.md`](./examples/aurora/CHANGELOG.md) (Aurora demo history)

## [1.7.4] — 2026-05-22 (round-5 falsification pass)

**Round-5 falsified v1.7.3.** Same Lesson 15 defect class (defensive
patches must cover ALL entry points) caught a SECOND time — round-5
found that `run --no-build`'s flash phase had its own `idf_runner`
call without the MSys check, even though build phase + flash command
were both fixed in v1.7.3.

### Critical (R3-CRIT regression — 2nd sibling path)

- **`esp-harness run --no-build` from Git Bash silently no-op'd.**
  `idf.py` exits 0 with the MSys/Mingw refusal, `wrote_bytes:0`,
  `verified:false`, but JSON reports `ok:true` and the composite
  command finishes claiming success. `run.py` flash phase now
  mirrors `flash.py`'s two-tier defence: substring match on the
  refusal message AND `wrote_bytes==0 && rc==0` sanity gate.

### Blocking

- **`esp-harness run` lost `patches.apply_all()` retry logic.**
  `build.py` auto-patches `waveshare__qmi8658` and retries on
  `i2c_master` link failures; `run.py` did not. AI agent following
  AGENT.md's "use `run` for one-shot iterations" hit BUILD_FAILED
  on first call against a fresh checkout. Now run.py imports +
  invokes `patches` with identical pre-build apply + post-failure
  retry behaviour.
- **`smoke.ps1` triple-trap case hardcoded `$aurora="D:\Code\esp-harness\examples\aurora"`.**
  Round-5 caught it: any other-machine run tested the maintainer's
  main dev tree, not the checked-out tree under test. Now uses
  `$RepoRoot\examples\aurora`. Also extended the loop to cover the
  `run --no-build` variant round-5 attacked.
- **`smoke.ps1` pytest gate failed on fresh clones** because
  `test_sim_diff_all_scenes_pass` correctly skips when the sim binary
  isn't built (`2 passed, 1 skipped`), but smoke insisted on
  `3 passed`. Now accepts the legitimate skip case; only `failed`
  is hard-error.

### Smoke gate

**7/7 host cases green** (was 7/7 too, but the build/flash/run
triple-trap case now exercises FOUR invocation forms including
`run --no-build` — what previously slipped through is now locked in).

### Convergence trajectory

| Round | Mode | Critical | Released |
|---|---|---|---|
| Author E2E | — | 5 | v1.7.1 |
| Subagent 1 | verify | 3 | v1.7.1.x |
| Subagent 2 | verify | 3 | v1.7.1.x |
| Subagent 3 | verify | 1 | v1.7.2 |
| Subagent 4 | falsify | 2 (R3-regression + scaffold-rot) | v1.7.3 |
| **Subagent 5** | **falsify + process audit** | **1 (R3-regression-2)** | **v1.7.4** |

**Critical now 1 → ?**. Round-5's process audit found exactly the
class of bug it predicted: previous round patched the file it knew
about, missed the sibling code path. v1.7.4 enumerates ALL idf_runner
entry points explicitly: build, flash, run-build-phase,
run-flash-phase. Each gated separately in smoke.

If a round-6 falsification finds zero critical, we've converged.

## [1.7.3] — 2026-05-22 (round-4 falsification pass)

**Round-4 falsified the v1.7.2 release.** A convergence-verification
subagent ran each v1.7.2 claim against the code on a fresh clone,
plus pushed on edges rounds 1-3 didn't cover. Six concrete defects
were on the v1.7.2 tag and the release notes' "0 round-3 criticals
open" claim was wrong on two counts.

### Critical (R3-CRIT regression — re-opened by round-4)

- **`esp-harness run` from Git Bash silently flashed stale binaries.**
  R3 patched `build.py` to detect the MSys/Mingw refusal, but
  `run.py`'s build phase did its own `idf_runner.run_idf_streaming`
  call without the check, and `flash.py` likewise. So the R3 trap
  was alive in the most-used composite command. Both patched.
- **`esp-harness flash` claimed `ok: true` with `wrote_bytes: 0`** from
  Git Bash — same root cause, same fix.

### Critical (genuinely new)

- **`esp-harness init` (deprecated alias) scaffolded an unbuildable
  project.** The template CMake hardcoded `AURORA_HARNESS_DIR` to
  `../esp32-harness-showcase/components` — a path that hasn't
  existed since the v1.5 monorepo flip. **`esp-harness init`'s
  default output had been broken for THREE releases** (v1.5, v1.6,
  v1.7.0-2) before round-4 caught it. Now forwards to
  `esp-harness new --component-source link` with a deprecation
  warning.

### Blocking

- **`?keys press pwr <hold_ms>` never auto-released.** v1.7.2 wired
  the synth-override expiry check for `boot` and `user` in
  `keys_task` but omitted the same check for `pwr`. Plus the PMIC
  IRQ_STATUS_2 handler unconditionally cleared `pwr_pressed` on
  any latched bit — fighting the synth even when the window was
  active. Both paths now honour the override.
- **v1.7.2 tag literally reported as `1.7.1`.** `pyproject.toml`
  wasn't bumped at tag time. `esp-harness --version` and
  `manifest.toolkit_version` both returned the stale string from
  the installed package metadata. Bumped to `1.7.3` here.
- **`install.ps1` missed the `[test]` extras** — fresh-clone smoke
  was 19/20 (pytest absent from venv). Now installs with extras.
- **`esp32-harness-showcase` reference sweep was narrow** — round-4
  grep returned 24 hits across 18 files; round-3's sweep only
  touched one. This commit hits idf_component.yml (url + issues),
  toolkit README + AGENT.md, sim-base/INTEGRATION.md, doctor.py
  comments, sim.py + screenshot.py docstrings, examples/aurora
  README + AGENT.md. Predecessor links in the top-level README's
  "Predecessors" row and similar archived-link contexts are
  preserved as legitimately historical.

### Smoke gate

**21/21 cases green**. Material changes:
- Triple-trap case: `build/flash/run refuse MSys/Mingw exit-0`.
- Keys-press case now exercises all three buttons (boot/user/pwr).
- `--wait-evt no-match returns timed-out evt_wait_ms (R4-edge)` —
  proactive pre-emption of round-4's edge tasklist.

### Convergence trajectory

| Round | Defects found | Critical | Released as |
|---|---|---|---|
| Author E2E (v1.7.0→1) | 8 | 5 | v1.7.1 |
| Subagent 1 | 8 | 3 | v1.7.1.x |
| Subagent 2 | 4 | 3 | v1.7.1.x |
| Subagent 3 | 10 | 1 | v1.7.2 |
| Subagent 4 (this) | 6 | 2 (1 regression + 1 fresh) | v1.7.3 |

Pattern observation: round-4 was the first round explicitly given a
**falsification** mandate (vs verify). That immediately surfaced
the run/flash R3-regression that the verify-mode rounds 1-3 had
missed. The 'critical drops to zero' target requires both falsification
AND verification rounds.

## [1.7.2] — 2026-05-22 (post-1.7.1 adversarial training pass)

**Adversarial-subagent convergence pass.** Three rounds of evaluation
by minimal-context subagents simulating first-time users surfaced
**12 defects** beyond the eight that v1.7.1's author E2E pass had
caught. All fixed; trajectory positive (8 → 4 → TBD criticals per
round); the 18-case smoke gate now locks every defect class in as a
permanent regression test.

### Round-1 findings (8 defects)

Scaffold path was unbuildable for two distinct reasons:
- **`CONFIG_IDF_TARGET_ESP32S3=y` missing** — fresh `idf.py build`
  defaulted to plain `esp32` (no PSRAM, smaller IRAM); the LVGL
  framebuffer alloc failed. (Lesson 5.)
- **`CONFIG_LV_USE_SNAPSHOT=y` missing** — `aurora-harness/src/screenshot.c`
  uses `lv_snapshot_take_to_draw_buf`, which is gated off by default
  in upstream LVGL Kconfig.
- **Vendor mode didn't bundle a BSP** — `bsp_display_start` from
  `bsp/esp-bsp.h` was unresolved at link, every fresh-`new` project
  failed to build. (Lesson 13.)
- Default `--component-source` flipped from `vendor` to `link`; the
  Waveshare BSP now wires in automatically via
  `EXTRA_COMPONENT_DIRS`. Out-of-the-box build works.

Polish:
- **Windows mojibake** (`?ping �� OK`) — CLI now forces stdout/stderr
  to UTF-8 + `chcp 65001` on entry.
- **Version triangulation** — `--version` was a stale string literal in
  `__init__.py`. Now reads from `pyproject.toml` via
  `importlib.metadata`. (Lesson 14.)
- **Manifest scene metadata** — 14 of 20 scenes had empty
  `description` / `tags`. All filled in.
- **monitor `--until "ready"`** — documented the substring-match
  gotcha (false-positive on "already").

### Round-2 findings (4 defects)

- **`manifest.device.available` was an 18-element list, not a bool** —
  `X and (list or list)` returns the operand. `bool()` wrap.
  (Lesson 11.) Smoke: `manifest.device.available is a real bool
  (R2-bug regression)`.
- **`console --wait-evt REGEX` missed pre-ack EVT** — gated matching
  on `ack_seen`, so EVTs emitted during command processing (e.g.
  `scene next` → `scene_changed`) landed in `events` but never
  matched. Every EVT now runs through the regex. (Lesson 12.) Smoke:
  `scene next --wait-evt captures pre-ack EVT (R2-bug regression)`.
- **`esp-harness new --component-source vendor` lied about being
  buildable** — said "cd / build" but build would fail (no BSP).
  Per-mode next-steps message now honest:
    - `link`: cd / build / flash (works as-is)
    - `vendor`: copy BSP first, with exact path
    - `depend`: registry-not-published, use link or vendor
  (Lesson 13.)
- **`esp-harness test` failed with `No module named pytest`** —
  pytest is in `[project.optional-dependencies].test`, not the
  default install. Now probes pytest first, prints three concrete
  install commands when missing.

### Drift cleanup

- README "Current release", `tools/esp-harness/README.md` badge, and
  the scaffolder's `idf_component.yml` pin (`^1.5.0`) were all stale.
  Bumped to `1.7.1+`; the scaffolder pin now reads `__version__` via
  `_pinnable_version()` so it auto-tracks releases. (Lesson 14.)
- Bench baseline regenerated against v1.7.1 firmware (was captured
  against `643b6fb-dirty` with the L2-fixed audio peak_dbfs=0.0 bug
  still in it).
- `examples/aurora/docs/scenes-map.md` gained rows XIX (Notify) and
  XX (Track) — was capped at XVIII.
- `examples/aurora/AGENT.md` "19 scenes" → "20 scenes".
- `docs/getting-started.md` "v1.5.0" → "v1.7.1".
- `docs/faq.md` "all three are 1.5.0" → tag/CLI sync at 1.7.1.
- `sim-base/mock_bsp.h` `bsp_display_lock` signature `void` → `bool`,
  matching `harness/bsp_iface.h`.
- `.github/workflows/sim-diff.yml` installs the toolkit with
  `[test]` extras so pytest is in the venv (was an undeclared
  dependency on the GitHub runner image).
- Top-level README points Windows users at `install.ps1` and the
  `python -m esp_harness` fallback for when `pip install -e` leaves
  the shim off `PATH`.

### Smoke gate (`tools/smoke.ps1`)

18 cases all green. New since v1.7.1:
- Version triangulation (`--version == manifest != 1.5.0`)
- L9 regression (`tap --wait-evt captures tap_hit`)
- R2 regression × 3 (pre-ack EVT, manifest available bool,
  bench --compare structured diff)

Companion `tools/smoke.sh` for Linux/Mac (host-only gates).

### `docs/lessons-v1.7.md`

Lessons L11-L14 appended. Convergence summary now tracks all three
rounds with per-round defect counts + severities.

### Round-3 findings (1 critical + 4 blocking + 5 minor)

Round-3 subagent (port-pretend / AI-agent E2E loop / sim regression
hunt / doc drift) on a fresh clone:

| Defect | Severity | Lesson |
|---|---|---|
| **Git Bash silent-build trap** — `idf.py` exits 0 with 'MSys/Mingw is no longer supported' from Git Bash on Windows. build.py used to accept rc=0 as success; AI agents would flash stale binaries with no warning. | critical | L15 (new) |
| **No synthetic-keypress** — physical BOOT/USER/PWR buttons couldn't be exercised remotely. | blocking | — |
| **`sim/README.md` claimed 3 scenes** (Halo/Grid/Bloom) — actually 13 of 20. | blocking | — |
| **`components/aurora-harness/README.md`** referenced 'esp32-harness-showcase' (pre-monorepo). | blocking | — |
| **`tools/esp-harness/tests/README.md`** stale pytest path + CI workflow location. | blocking | — |
| 5 minor: stale `1.5.0` in docs/index.html + svg + architecture.md / 6 broken markdown links / PORTING.md doesn't mention `bsp_display_start()` + `bsp/esp-bsp.h` convention. | minor | — |

All fixed:
- **`build.py`** detects the MSys/Mingw refusal + ELF mtime sanity gate;
  fails closed with `exit_code=100` and a clear hint. Smoke gate
  `build refuses MSys/Mingw exit-0 (R3-CRIT regression)` (host-only,
  skips if bash not on PATH).
- **`?keys press <name> [HOLD_MS]`** — new console command + `keys_synth_press()`
  in firmware. Synth override with expiry tick lets `keys_task` skip
  overwriting `pressed=true` for the hold window; count counter +
  `pressed` level both honour synth. Smoke gate `?keys press boot
  synth (R3-bug regression)`.
- **PORTING.md** now documents the `bsp/esp-bsp.h` include-path
  convention + `bsp_display_start()` entry-point (both load-bearing
  for a fresh port; both omitted previously).
- All five doc/path fixes applied in single commit `9f0b56b`.

### Status

- **Smoke: 20/20 cases green** (6 host + 14 device).
- All round-1, round-2, **and round-3** findings: fixed and
  regression-tested.
- Convergence trajectory: 8 → 4 → 1 critical per round. The
  framework is at the point where further adversarial rounds find
  papercuts rather than blockers.

## [1.7.1] — 2026-05-22

**Quality convergence wave.** Hardware verification of v1.7.0 surfaced
eight defects across audio / wifi / build infrastructure. Fixed all
of them, captured the root causes in `docs/lessons-v1.7.md`, and
shipped a `tools/smoke.ps1` pre-release gate so every defect class
gets a permanent regression test.

### Defects fixed

- **`audio tone` reported `bytes:0`** despite playing — `esp_codec_dev_write`
  returns `esp_err_t` (0 = OK) not POSIX byte count. Total-bytes accounting
  was summing zeros forever.
- **`audio mic` / `loopback` reported `peak_dbfs:0.0`** (full-scale) in
  silent rooms — first DMA buffer after `_open` is stale garbage. Added
  throwaway read + `INT16_MIN abs` saturation cap.
- **`wifi_init` returned `ESP_ERR_NO_MEM`** even with 39 KB free DRAM —
  `STATIC_TX_BUFFER_NUM=16` (default) reserved ~27 KB by itself; trimmed
  to 6 (Aurora is RX-heavy: scan + OTA download).
- **`wifi disconnect` immediately auto-reconnected** — event handler's
  3-retry loop didn't distinguish user intent. New `s_user_disconnect`
  flag.
- **Console tokenizer didn't honour `"..."`** — `wifi connect ssid="My AP"`
  failed. Added quote-stripping with no backslash escape (kept tiny).
- **`MBEDTLS_INTERNAL_MEM_ALLOC=y`** routed mbedtls heap to scarce DRAM —
  switched to PSRAM (`MBEDTLS_EXTERNAL_MEM_ALLOC=y`).
- **`SPIRAM_MALLOC_ALWAYSINTERNAL=1024`** kept too many small allocs in
  DRAM — dropped to 128.
- **`ble_release_memory()`** — boot-time BLE-never-init path didn't free
  the BT controller's DRAM pool. New function + `radio wifi` calls it
  unconditionally.

### Quality infrastructure

- **`docs/lessons-v1.7.md`** — ten lessons with the format `What broke /
  Root cause / Why we missed it / Process change`. Reference for future
  releases.
- **`tools/smoke.ps1`** — pre-release gate. Host gates (doctor / pytest /
  sim-diff / manifest) plus device gates (ping / stat / sys / audio
  tone bytes > 0 / audio mic peak in [-90,-10] / OTA info / scene
  switch). Each defect from this wave has a permanent regression case
  labelled with its lesson number.
- **`.env` + `.gitignore`** — test credentials no longer live in chat
  transcripts.

### Verification

13 / 13 smoke cases green on hardware (4 host + 9 device).

## [1.7.0] — 2026-05-22

**Connectivity + real OTA.** Closes the gap between the v1.6 `?ota` skeleton
(info / mark-valid / rollback only) and a device that can actually pull a
new image over the air. Pairs with WiFi STA connect + NVS-backed
credential persistence so the loop is one command per side: connect once,
then `?ota download url=…` whenever a new build lands.

### Aurora firmware

- **`examples/aurora/main/peripherals/wifi_creds.{h,c}`** — NVS-backed
  credential store in namespace `wifi_cred`. Five-function API
  (`init` / `set` / `get` / `has` / `forget`). Credentials are stored as
  plaintext; NVS encryption is an opt-in via menuconfig for shipping
  builds.
- **`examples/aurora/main/peripherals/wifi.c`** — added STA-mode connect
  path alongside the existing scan. New API:
  - `wifi_connect(ssid, pass, timeout_ms) → bool` (event-group +
    `IP_EVENT_STA_GOT_IP` gate, 3 automatic retries on disconnect)
  - `wifi_disconnect()`, `wifi_is_connected()`, `wifi_get_status()`
- **`harness/harness_commands.c`** — `wifi` command rewritten as
  multi-subcommand dispatcher: `scan` / `connect ssid=… pass=… save=1` /
  `disconnect` / `forget` / `status`. `?ota` gains `download url=…`
  implemented via `esp_https_ota` streaming with integer-percent EVT
  progress emission. `?ota download` does *not* auto-reboot — the host
  decides when to flip slots (so the AI can screenshot, status-check,
  and then `?reset`).
- **`partitions.csv`** — switched from `factory 8M + storage 7M` to dual
  `ota_0 5M + ota_1 5M + storage 5M`. `otadata` (2 sectors) added.
- **`sdkconfig.defaults`** — added
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`,
  `CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y`, plus dev-friendly
  `CONFIG_OTA_ALLOW_HTTP=y` and `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y`
  (production should disable both and bundle a CA cert).

### Toolkit + build infra

- **`tools/esp-harness/src/esp_harness/core/patches.py`** — new module
  centralizing `managed_components/` patches. Exports
  `apply_all(project_dir)`, `KNOWN_PATCHES`, `RETRY_SIGNATURES`,
  `stderr_suggests_retry(stderr)`. First entry: the `waveshare__qmi8658`
  upstream bug pair (missing `esp_driver_i2c` REQUIRES + `M_PI` without
  `#ifndef` guard).
- **`build.py`** — applies patches pre-build (idempotent); on failure
  inspects stderr for known retry signatures and retries once. Documents
  the patches that were applied in the build output.
- **`examples/aurora/CMakeLists.txt`** — `esp_harness_apply_known_patches()`
  CMake function as a project-local fallback. Detects + patches
  `managed_components/waveshare__qmi8658/{CMakeLists.txt,qmi8658.h}`
  during configure. Idempotent via signature checks. Means a fresh
  clone builds without needing the toolkit at all.

### Verification

- target build: clean rebuild 65.5s, 0 warnings, all artifacts produced
- sim diff: 13 / 13 scenes identical (no UI regression)
- pytest: 3 / 3 passing (doctor / manifest / sim-diff)
- manifest: 17 toolkit commands surfaced

### Deferred to v1.8

- BLE-peripheral WiFi provisioning UI (NimBLE GATT receiver writing to
  the v1.7 `wifi_creds` API). API surface is ready; only the BLE side is
  missing.

## [1.6.0] — 2026-05-22

**Project maturity polish.** Documentation, branding, and onboarding
ecosystem brought to a level where a fresh reader — or an author
returning after total amnesia — can get oriented in under 10 minutes.

### Brand + visual identity (`docs/brand/`)

- Full brand assets: `logo.svg` (light), `logo-dark.svg` (dark variant),
  `wordmark.svg` (mark + serif wordmark), `favicon.svg` (32 px tab icon),
  `social-card.svg` (1280×640 GitHub OG image). Plus a `brand/README.md`
  documenting the mark, colors, typography, don'ts.
- The mark: an "H" reframed as a control harness — two black verticals
  (device side), one rust cross-bar (dev-loop signal), a paper dot at
  center (manifest discovery point), a dashed outer frame (device
  boundary).

### Docs ecosystem (`docs/`)

- **`manifesto.md`** — the "why" doc. Design philosophy, what we
  believe, explicit out-of-scope list, what "done" looks like per layer,
  how we make decisions. Built to survive the amnesia test.
- **`faq.md`** — 16+ common questions: what is it, why not PIO, do I
  need the hardware, sim vs device fidelity, version pinning, license.
- **`troubleshooting.md`** — symptom-based fixes: qmi8658 patch, M_PI
  redefinition, SDL2 link order, target detection, sim build issues,
  Python install issues. Each entry has the exact command.
- **`diagrams/`** — three SVG architecture diagrams: three-layer stack,
  dev loop concentric speeds, repo layout. Inlined in homepage,
  standalone for slides.
- **`index.html`** — proper homepage with hero / terminal / three paths
  / architecture / comparison table. Used as the GitHub Pages entry
  point. Same editorial design language as the progress report.

### Root README → landing page

Rewrote root `README.md` as a proper landing page: logo + badges, "What
this is" table, 30-second quickstart, three-path onboarding, comparison
vs raw ESP-IDF / PlatformIO / Arduino / LVGL official sim, status block.

### New example (`examples/hello-minimal/`)

The smallest possible esp-harness consumer (one scene, one console
command, no peripherals). Proves the `esp-harness new` template
integrity and documents "what the floor looks like" for new projects.
Uses `link` vendoring mode with the path resolved relatively.

### Contributing infrastructure (`.github/`)

- **`CONTRIBUTING.md`** — setup, dev loop, what we accept / decline,
  PR style, what good looks like.
- **`ISSUE_TEMPLATE/{bug,feature,question}.md`** + `config.yml` —
  structured issue intake with environment info pre-prompted.
- **`PULL_REQUEST_TEMPLATE.md`** — checklist including artifact
  affiliation, semver bump on component change, manifest update on
  new toolkit command.

### Per-artifact README polish

Each of `components/aurora-harness/`, `tools/esp-harness/`, and
`examples/aurora/` README now has a badge bar at the top showing its
version / dependencies / status. Cross-link to the monorepo root and
to each other. Old "esp32-harness-showcase as sibling repo" framing
removed.

### GitHub repo metadata

Via `gh repo edit`: description set, homepage URL set, 14 topics
added (esp32 / esp-idf / esp32-s3 / lvgl / embedded / firmware /
ai-driven / developer-tools / scaffolding / console-protocol /
monorepo / sdl2 / simulator / harness). Wiki disabled.

### GitHub Pages

Source: `master` branch, `/docs` path. Serves `docs/index.html` at
https://caldis.github.io/esp-harness/.

### Status

- Verified post-changes: `esp-harness doctor` 8/8 OK · `esp-harness
  test` 3/3 PASS · `esp-harness manifest` 17 toolkit cmds.
- All Phase J deliverables in this single release.

## [1.5.0] — 2026-05-21

**Monorepo migration.** Merges the v1.0–v1.4 history of two predecessor
repos into a single repo with clean artifact boundaries.

### Restructured

The two predecessor repos:

- [`esp32-harness-showcase`](https://github.com/Caldis/esp32-harness-showcase) (v1.0.0 → v1.4.0)
- [`esp32-harness-toolkit`](https://github.com/Caldis/esp32-harness-toolkit) (v1.0.0)

become five top-level artifact types in this repo:

```
esp-harness/
├── components/aurora-harness/         was showcase/components/aurora-harness/
├── tools/esp-harness/                  was esp32-harness-toolkit/
├── examples/aurora/                    was showcase/main/ + showcase/sim/
├── sim-base/                           NEW: extracted from sim/'s generic half
│                                       (ESP-IDF stubs, mock_bsp, INTEGRATION.md)
├── boards/esp32_s3_touch_amoled_2_16/  was showcase/components/<same name>
└── docs/                               cross-cutting: architecture, getting-
                                        started, harness-report.html
```

### New

- **Root `README.md`** — three-path onboarding funnel (use the library /
  run the demo / try the simulator / contribute).
- **Root `AGENT.md`** — cross-cutting AI-session entry point.
- **`docs/architecture.md`** — three-layer mental model + design
  rationale + simulator architecture.
- **`docs/getting-started.md`** — concrete 15-minute onboarding for the
  three main user personas.
- **`sim-base/`** — extracted generic half of the sim build (ESP-IDF
  stub headers, mock_bsp, prereq detector). Aurora's `examples/aurora/sim/`
  now consumes it; future examples reuse the same base.
- **`esp-harness new <name>`** command — replaces `esp-harness init`
  (kept as alias). Three vendoring modes:
  - `--component-source vendor` (default) copies aurora-harness into
    the new project's `components/`. Self-contained.
  - `--component-source link` references the monorepo via
    `EXTRA_COMPONENT_DIRS`. Lighter footprint.
  - `--component-source depend` writes an `idf_component.yml`
    dependency. Requires Registry publication (TBD).
  - Generated projects include a project-specific `AGENT.md`.
- **CI**: `.github/workflows/sim-diff.yml` now runs from a single
  checkout (previously cross-cloned two repos).

### Changed

- **Path references** throughout — toolkit auto-detection logic, CI
  workflow, init template, sim build's `ESP_HARNESS_ROOT` variable
  all rewritten to assume monorepo layout. Legacy sibling-repo paths
  retained as fallback in `_default_sim_binary` and `_check_harness_component`
  for transition users.
- **`examples/aurora/sdkconfig.defaults`** — `CONFIG_IDF_TARGET="esp32s3"`
  added explicitly so a fresh clone defaults to the right target
  without needing `idf.py set-target esp32s3` first.
- **`examples/aurora/CMakeLists.txt`** — adds
  `-Wno-error=builtin-macro-redefined` to handle gcc 15.2+ M_PI
  redefinition warning from `waveshare__qmi8658` header.
- **`tools/esp-harness/pyproject.toml`** — version bumped 0.1.0 → 1.5.0
  (synced with monorepo), expanded metadata (classifiers, keywords,
  URLs, optional `[test]` dep group).

### Known issues

- **`waveshare__qmi8658` upstream bug** — its `CMakeLists.txt` REQUIRES
  only the legacy `driver` umbrella, missing `esp_driver_i2c` (split out
  in IDF v6). Same for `M_PI` macro redefinition without `#ifndef` guard.
  Both require post-fetch patches to `managed_components/` on first
  build. A clean monorepo CMake auto-patch hook is on the v1.6 roadmap.
- **`esp-harness init`** (legacy alias) still generates the old
  sibling-repo template. New code should use `esp-harness new`.

### Predecessor history preserved

Both old repos remain as archives; their git logs hold the v1.0-v1.4
detailed commit history. This repo's history starts fresh at v1.5.0.
