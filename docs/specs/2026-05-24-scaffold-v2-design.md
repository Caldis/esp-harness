# esp-harness Scaffold v2: Protocol-First Agent Platform

**Date:** 2026-05-24
**Status:** Design approved, pending implementation plan
**Approach:** Protocol-First (Approach A)

## Problem

AI agents working on ESP32 projects cannot reliably discover what tools are available, what the project is configured for, or how to execute a complete development cycle. This leads to:

- Agents "forgetting" toolchain capabilities across sessions (e.g., not knowing `esp-harness screenshot` exists)
- Hardcoded assumptions about ports, boards, and paths that rot between sessions
- No single command to execute the full build-flash-verify loop
- Module installation requires manual file creation and wiring

The current `esp-harness new` generates a minimal skeleton but doesn't establish the project as a self-describing agent endpoint.

## Design Principles

1. **Protocol over documentation.** Agent discovers capabilities via `manifest --json`, not by reading docs. If it's not in the manifest, it doesn't exist.
2. **Zero-memory operation.** Every session starts with `esp-harness manifest --json`. No prior knowledge required. No reliance on memory systems or conversation history.
3. **One-command cycle.** `esp-harness cycle` executes the full build-flash-verify loop. Agent writes code, runs one command, reads structured output.
4. **Declarative modules.** `harness.json` declares what's enabled. `esp-harness add/remove` mutates both filesystem and config atomically.
5. **Backward compatible.** Projects without `harness.json` fall back to CLI args. Existing projects keep working.

## Architecture

### harness.json

The project's single source of truth. Every CLI command reads defaults from this file.

```json
{
  "$schema": "https://esp-harness.dev/schema/v1.json",
  "name": "my-agent-dashboard",
  "version": "0.1.0",
  "board": "esp32_s3_touch_amoled_2_16",
  "port": "COM9",
  "modules": {
    "scenes": true,
    "console": true,
    "toast": true,
    "bridge": false,
    "hooks": false,
    "sim": false,
    "ota": false,
    "push-banner": false,
    "buttons": false
  },
  "agent": {
    "bootstrap": "esp-harness manifest --json",
    "verify": "esp-harness verify",
    "cycle": ["build", "flash", "verify"]
  }
}
```

**Context inference chain:** All CLI commands (build, flash, screenshot, console, etc.) read `harness.json` for defaults. `esp-harness flash` with no args resolves port from `harness.json.port`. If `harness.json` doesn't exist, fall back to CLI args (backward compat).

### Module System

Modules are installable units that add capability to a project.

```bash
esp-harness add <module>       # install: create files + update harness.json
esp-harness remove <module>    # uninstall: remove files + update harness.json
esp-harness list-modules       # show available and installed modules
```

**Core module registry:**

| Module | Default | Provides |
|--------|---------|----------|
| `scenes` | yes | scene_framework.h + scene_hello.c template |
| `console` | yes | console_protocol.h + default_cmds (stat, dump, scene) |
| `toast` | yes | harness_toast overlay |
| `bridge` | no | Python bridge template + hook_dispatch.py |
| `hooks` | no | Claude Code / Cursor hook config snippets |
| `sim` | no | SDL2 host simulator + golden baselines |
| `ota` | no | WiFi OTA + signature verification scaffold |
| `push-banner` | no | lv_layer_top tool-event overlay |
| `buttons` | no | BOOT/USER GPIO button driver + handler |

Each module is a Python class implementing:
- `scaffold(project_dir: Path)` -- create files and directories
- `remove(project_dir: Path)` -- remove files and clean up
- `manifest_fragment() -> dict` -- contribute to manifest output
- `cmake_requires() -> list[str]` -- ESP-IDF component dependencies

### CLI Changes

**New commands:**

| Command | Purpose |
|---------|---------|
| `esp-harness create <name>` | Generate project with harness.json + skeleton (replaces `new`) |
| `esp-harness add <module>` | Install module to project |
| `esp-harness remove <module>` | Uninstall module |
| `esp-harness list-modules` | Show available/installed modules |
| `esp-harness verify` | Screenshot + sim diff + structured pass/fail output |
| `esp-harness cycle` | Execute agent.cycle from harness.json (default: build+flash+verify) |

**Enhanced commands (read harness.json defaults):**

All existing commands (`build`, `flash`, `screenshot`, `console`, `monitor`, `manifest`) gain automatic context from `harness.json`. Example: `esp-harness flash` with no `--port` reads `harness.json.port`.

**`esp-harness cycle` behavior:**

```
$ esp-harness cycle --json
{"step":"build","status":"ok","elapsed_ms":12400}
{"step":"flash","status":"ok","elapsed_ms":8200}
{"step":"verify","status":"ok","screenshot":".harness/latest.png","diff":"none"}
```

On failure, exits with the semantic code of the failing step (20=build, 30=flash, etc.) and the JSON includes `"error"` field.

### Manifest Enhancement

Current manifest: toolkit_commands + device.commands + device.scenes.

Enhanced manifest adds project context:

```json
{
  "toolkit_version": "2.0.0",
  "project": {
    "name": "my-dashboard",
    "board": "esp32_s3_touch_amoled_2_16",
    "port": "COM9",
    "modules": ["scenes", "console", "toast", "bridge"],
    "cycle": ["build", "flash", "verify"]
  },
  "toolkit_commands": [ "..." ],
  "module_commands": {
    "bridge": {
      "start": "python bridge/bridge.py serve",
      "stop": "Stop-Process on port 7321"
    }
  },
  "device": {
    "available": true,
    "commands": [ "..." ],
    "scenes": [ "..." ]
  },
  "convention": {
    "bootstrap": ["Run this manifest at session start."],
    "cycle": "esp-harness cycle",
    "add_scene": "Create main/scenes/scene_<name>.c, register in app_main.c, rebuild.",
    "add_command": "console_protocol_register() in app code. Auto-surfaces in manifest."
  }
}
```

### CLAUDE.md Auto-Generation

`esp-harness create` generates a CLAUDE.md that points to manifest, not to itself:

```markdown
# CLAUDE.md

## Bootstrap
Run `esp-harness manifest --json` to discover all project capabilities.
Do this at the start of every session.

## Development Cycle
Run `esp-harness cycle` after code changes (build + flash + verify).

## Adding Features
- New scene: create `main/scenes/scene_<name>.c`, register in `app_main.c`
- New command: `console_protocol_register()` -- auto-surfaces in manifest
- New module: `esp-harness add <module>`

## Key Files
- `harness.json` -- project config (board, port, modules)
- `main/app_main.c` -- entry point
- `main/scenes/` -- UI scenes
```

### Project Template

`esp-harness create my-project` generates:

```
my-project/
├── harness.json
├── CLAUDE.md
├── README.md
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
├── main/
│   ├── CMakeLists.txt
│   ├── app_main.c
│   └── scenes/
│       ├── scenes.h
│       └── scene_hello.c
├── .harness/
│   ├── latest.png
│   └── golden/
└── .gitignore
```

Interactive mode prompts for board and port (with auto-detect). `--json` mode uses defaults or CLI args for non-interactive scaffolding.

## Agent Full-Autonomy Loop

The end-to-end flow an agent executes with zero human intervention:

```
1. esp-harness manifest --json          → know everything
2. [write code: scene, command, bridge]
3. esp-harness cycle --json             → build + flash + verify
4. [read structured result]
5. if fail: read error, fix, goto 3
6. if pass: esp-harness screenshot      → capture for evidence
7. git commit
```

Every step has structured JSON output. Every failure has a semantic exit code. The agent never needs to guess, remember, or read documentation.

## Migration Path

Existing projects (like esp32-agent-dashboard) adopt incrementally:

1. Add `harness.json` to project root (manual or `esp-harness init --from-existing`)
2. Existing CLI commands start reading defaults from it
3. `esp-harness add` becomes available for new modules
4. CLAUDE.md can be regenerated with `esp-harness init --claude-md-only`

No breaking changes to existing workflows.

## Success Criteria

1. A new agent (Claude Code, Codex, Cursor) entering a scaffolded project can, with zero prior knowledge, execute a complete build-flash-verify cycle by following only `harness.json.agent.bootstrap`.
2. `esp-harness cycle` succeeds end-to-end in under 60 seconds on a warm build.
3. Adding a new scene is a 3-step process: create file, register, `esp-harness cycle`.
4. The manifest is the complete API surface -- nothing exists outside it.
5. Removing `harness.json` doesn't break any existing command (graceful fallback).

## Out of Scope (v2)

- Remote device management (multi-device fleet)
- Cloud-hosted build (all local)
- Package registry for publishing modules (local-only for now)
- BLE/WiFi transport (stays on USB-Serial for v2)
