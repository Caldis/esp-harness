# Changelog

Repo-level milestones. Per-artifact changelogs live in:

- [`components/aurora-harness/`](./components/aurora-harness/) (component history; preserved from `esp32-harness-showcase` v1.0-v1.4)
- [`tools/esp-harness/CHANGELOG.md`](./tools/esp-harness/CHANGELOG.md) (toolkit history; preserved from `esp32-harness-toolkit`)
- [`examples/aurora/CHANGELOG.md`](./examples/aurora/CHANGELOG.md) (Aurora demo history)

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
