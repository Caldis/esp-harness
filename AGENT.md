# AGENT.md — esp-harness root onboarding for AI sessions

> If you're an AI dropped into this monorepo for the first time, read this
> first. The README is for humans; this is for you.

## 0. Bootstrap (first 30 seconds)

```bash
esp-harness doctor    # → 8-check env health, JSON in --json mode
esp-harness manifest  # → enumerates all 16+ toolkit cmds, all device
                      #   cmds, all device scenes in one document
```

**Convention**: if a capability isn't in `manifest`, it doesn't exist for
you. Don't grep firmware source looking for it. If you find something
useful by grepping, *register it* (add to `?help` or `TOOLKIT_COMMANDS`)
so the next session also sees it.

## 1. Monorepo layout (where everything lives)

```
esp-harness/
├── components/aurora-harness/      ← REUSABLE C library (LVGL scene fw +
│                                     console protocol + toast + progress +
│                                     screenshot + default cmds + bsp_iface)
│                                     This is the load-bearing piece.
│                                     See components/aurora-harness/README.md
│                                     and AGENT.md (inside the component dir).
│
├── tools/esp-harness/              ← Python CLI: build/flash/monitor/sim/
│                                     bench/manifest/doctor/test/new/...
│                                     See tools/esp-harness/AGENT.md.
│
├── examples/aurora/                ← Reference firmware. The DEMO + the
│                                     reference for "how to use aurora-
│                                     harness". Has its own AGENT.md.
│
├── sim-base/                       ← Generic half of host LVGL build.
│                                     ESP-IDF stub headers + mock_bsp.
│
├── boards/<vendor-model>/          ← Vendored BSP components.
│                                     Aurora consumes esp32_s3_touch_amoled_2_16
│                                     via main/CMakeLists.txt REQUIRES.
│
└── docs/                           ← Cross-cutting docs (architecture,
                                      getting started, progress report).
```

Each artifact directory has its own README + AGENT (where relevant).
This root AGENT.md only covers cross-cutting concerns.

## 2. Where to put new things (decision table)

| Doing this... | Goes in... | Why |
|---|---|---|
| Reusable C primitive (toast, progress, console cmd) | `components/aurora-harness/src/` | The library |
| Aurora-specific scene / peripheral / console cmd | `examples/aurora/main/...` | App glue, not reusable |
| New Python CLI subcommand | `tools/esp-harness/src/esp_harness/commands/` | Register in cli.py + manifest.py |
| New peripheral driver | `examples/<your-thing>/main/peripherals/` | Board / project specific |
| Host stub for a peripheral (sim) | `examples/<your-thing>/sim/mock_peripherals.c` | Signature must match real header |
| Generic sim infrastructure (more ESP stubs etc.) | `sim-base/include/` | Shared across examples |
| A new vendor BSP (we'd ship) | `boards/<underscored_dir>/` | ESP-IDF requires dir-name = component-name |
| New full firmware example | `examples/<name>/` | Self-contained: own main/, CMakeLists, etc. |
| Cross-artifact docs | `docs/` | Architecture, conventions |

## 3. Cardinal rules

1. **Don't break the manifest.** Every new toolkit command goes in
   `TOOLKIT_COMMANDS` (`tools/check_manifest.py` lints this). Every new
   firmware command goes via `console_protocol_register()` (auto-surfaces
   in `?help json`). Every new scene goes via `scene_fw_register()` (auto-
   surfaces in `scene list`).
2. **Each commit verifies.** Run `esp-harness test` (3 pytest tests + sim
   diff) before push. CI runs the same on every PR.
3. **Documentation lives next to code.** Component README in
   `components/aurora-harness/README.md`. Aurora README in
   `examples/aurora/README.md`. Don't dump everything into top-level docs.
4. **Examples are independent.** `examples/<name>/` must build with
   `idf.py build` from its own directory. No "you need to run X first"
   from outside the example dir (other than installing the toolkit).
5. **Component is consumer-agnostic.** Anything you add to
   `components/aurora-harness/` must work for unknown future consumers —
   no `#include "ui_shell.h"` from Aurora, no peripheral references,
   no hardcoded screen dimensions.

## 4. Common workflows

### Make a UI change in Aurora
```bash
# edit examples/aurora/main/scenes/scene_foo.c
cd examples/aurora/sim
cmake --build build -j                                # ~3 s
esp-harness sim diff --scenes foo,grid,bloom          # ~5 s
# If intentional break:
esp-harness sim update-golden --scenes foo
# Then flash to device:
cd .. && esp-harness build && esp-harness flash
```

### Add a generic primitive to aurora-harness
```bash
# 1. Write components/aurora-harness/src/<name>.c + include/harness/<name>.h
# 2. Add to components/aurora-harness/CMakeLists.txt SRCS
# 3. Add a reference scene under examples/aurora/main/scenes/scene_<name>.c
# 4. Bump idf_component.yml version (semver)
# 5. esp-harness test  (manifest + sim diff)
```

### Add a toolkit command
```bash
# 1. tools/esp-harness/src/esp_harness/commands/<name>.py
# 2. Register in cli.py + add entry to commands/manifest.py::TOOLKIT_COMMANDS
# 3. esp-harness test  (check_manifest lint runs as part of manifest test)
```

### Bootstrap a new project consuming aurora-harness
```bash
esp-harness new my-thing --component-source vendor   # copy harness into project
# or:
esp-harness new my-thing --component-source link     # share via EXTRA_COMPONENT_DIRS
# or:
esp-harness new my-thing --component-source depend   # via component manager (TBD)
```

## 5. When in doubt

- "Does this belong in the component or the example?" → If it can work
  for an unknown future project, component. Otherwise example.
- "How do I find out what `?xxx` does?" → `esp-harness manifest`.
- "Why does this build fail?" → `esp-harness doctor` first.
- "Sim is broken / golden mismatch?" → `esp-harness sim diff --save-diffs
  D:\tmp\diffs` then visually inspect.
- "Tests fail unexpectedly?" → `cd tools/esp-harness && pytest tests -v`
  for fuller output.

## 6. Predecessor archives

This monorepo replaces two earlier repos. Their git histories are
preserved as the source of truth for the pre-v1.5 history:

- [`esp32-harness-showcase`](https://github.com/Caldis/esp32-harness-showcase) — v1.0.0 through v1.4.0
- [`esp32-harness-toolkit`](https://github.com/Caldis/esp32-harness-toolkit) — v1.0.0

When debugging "why was X done this way", check the CHANGELOG and git
log on those old repos for the longest history.
