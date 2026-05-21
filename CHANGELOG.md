# Changelog

Repo-level milestones. Per-artifact changelogs live in:

- [`components/aurora-harness/`](./components/aurora-harness/) (component history; preserved from `esp32-harness-showcase` v1.0-v1.4)
- [`tools/esp-harness/CHANGELOG.md`](./tools/esp-harness/CHANGELOG.md) (toolkit history; preserved from `esp32-harness-toolkit`)
- [`examples/aurora/CHANGELOG.md`](./examples/aurora/CHANGELOG.md) (Aurora demo history)

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
