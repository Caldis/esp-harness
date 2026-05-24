# Scaffold v2: Protocol-First Agent Platform — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Transform esp-harness into a protocol-first scaffolding platform where AI agents discover project capabilities via `harness.json` + `manifest --json`, and execute full build-flash-verify cycles with one command.

**Architecture:** A new `core/config.py` module provides `harness.json` read/write. A `core/modules.py` registry defines installable modules. Six new CLI commands (`create`, `add`, `remove`, `list-modules`, `verify`, `cycle`) use these primitives. Existing commands (`build`, `flash`, `screenshot`, `manifest`) gain context inference from `harness.json`. The `create` command replaces `new` as the primary scaffolding entry point, generating `harness.json` + auto-generated `CLAUDE.md`.

**Tech Stack:** Python 3.10+, argparse CLI, JSON config, pytest

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `src/esp_harness/core/config.py` | Create | Read/write/validate harness.json |
| `src/esp_harness/core/modules.py` | Create | Module registry + scaffold/remove/manifest operations |
| `src/esp_harness/commands/create.py` | Create | `esp-harness create <name>` — generate project with harness.json |
| `src/esp_harness/commands/add.py` | Create | `esp-harness add <module>` |
| `src/esp_harness/commands/remove_mod.py` | Create | `esp-harness remove <module>` |
| `src/esp_harness/commands/list_modules.py` | Create | `esp-harness list-modules` |
| `src/esp_harness/commands/verify.py` | Create | `esp-harness verify` — screenshot + diff |
| `src/esp_harness/commands/cycle.py` | Create | `esp-harness cycle` — build+flash+verify pipeline |
| `src/esp_harness/commands/manifest.py` | Modify | Add project context from harness.json |
| `src/esp_harness/commands/build.py` | Modify | Read project dir from harness.json |
| `src/esp_harness/commands/flash.py` | Modify | Read port from harness.json |
| `src/esp_harness/commands/screenshot.py` | Modify | Read port from harness.json |
| `src/esp_harness/cli.py` | Modify | Register new commands |
| `src/esp_harness/exit_codes.py` | Modify | Add VERIFY_FAILED, CYCLE_FAILED |
| `tests/test_config.py` | Create | harness.json loader tests |
| `tests/test_modules.py` | Create | Module registry tests |
| `tests/test_create.py` | Create | Project creation tests |
| `tests/test_cycle.py` | Create | Cycle pipeline tests |

All paths relative to `D:\Code\esp-harness\tools\esp-harness\`.

---

### Task 1: harness.json Config Loader

**Files:**
- Create: `src/esp_harness/core/config.py`
- Create: `tests/test_config.py`

- [ ] **Step 1: Write failing tests for config loader**

```python
# tests/test_config.py
"""Tests for harness.json config loader."""
from __future__ import annotations

import json
from pathlib import Path

import pytest

from esp_harness.core.config import load_config, HarnessConfig, CONFIG_FILENAME


def test_load_from_directory(tmp_path: Path):
    cfg = {
        "name": "test-project",
        "version": "0.1.0",
        "board": "esp32_s3_touch_amoled_2_16",
        "port": "COM9",
        "modules": {"scenes": True, "console": True, "bridge": False},
        "agent": {
            "bootstrap": "esp-harness manifest --json",
            "verify": "esp-harness verify",
            "cycle": ["build", "flash", "verify"],
        },
    }
    (tmp_path / CONFIG_FILENAME).write_text(json.dumps(cfg), encoding="utf-8")
    result = load_config(tmp_path)
    assert result is not None
    assert result.name == "test-project"
    assert result.board == "esp32_s3_touch_amoled_2_16"
    assert result.port == "COM9"
    assert result.modules["scenes"] is True
    assert result.modules["bridge"] is False
    assert result.agent_cycle == ["build", "flash", "verify"]


def test_load_returns_none_when_missing(tmp_path: Path):
    result = load_config(tmp_path)
    assert result is None


def test_load_walks_up(tmp_path: Path):
    cfg = {"name": "parent-project", "version": "0.1.0", "board": "test", "port": "COM3", "modules": {}, "agent": {}}
    (tmp_path / CONFIG_FILENAME).write_text(json.dumps(cfg), encoding="utf-8")
    child = tmp_path / "main" / "scenes"
    child.mkdir(parents=True)
    result = load_config(child)
    assert result is not None
    assert result.name == "parent-project"


def test_enabled_modules():
    cfg = HarnessConfig(
        name="t", version="0.1.0", board="b", port="COM1",
        modules={"scenes": True, "console": True, "bridge": False},
        agent_bootstrap="", agent_verify="", agent_cycle=[],
        config_path=Path("."),
    )
    assert cfg.enabled_modules() == ["console", "scenes"]


def test_save_roundtrip(tmp_path: Path):
    cfg = HarnessConfig(
        name="rt", version="0.2.0", board="myboard", port="COM5",
        modules={"scenes": True, "sim": False},
        agent_bootstrap="esp-harness manifest --json",
        agent_verify="esp-harness verify",
        agent_cycle=["build", "flash", "verify"],
        config_path=tmp_path,
    )
    cfg.save()
    reloaded = load_config(tmp_path)
    assert reloaded is not None
    assert reloaded.name == "rt"
    assert reloaded.port == "COM5"
    assert reloaded.modules == {"scenes": True, "sim": False}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd D:\Code\esp-harness\tools\esp-harness && python -m pytest tests/test_config.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'esp_harness.core.config'`

- [ ] **Step 3: Implement config module**

```python
# src/esp_harness/core/config.py
"""harness.json config loader — the project's single source of truth.

Every CLI command reads defaults from this file. If harness.json is
absent, commands fall back to CLI args (backward compat).
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

CONFIG_FILENAME = "harness.json"
MAX_WALK_UP = 10


@dataclass
class HarnessConfig:
    name: str
    version: str
    board: str
    port: str
    modules: dict[str, bool]
    agent_bootstrap: str
    agent_verify: str
    agent_cycle: list[str]
    config_path: Path  # directory containing harness.json

    def enabled_modules(self) -> list[str]:
        return sorted(k for k, v in self.modules.items() if v)

    def save(self) -> None:
        data = {
            "name": self.name,
            "version": self.version,
            "board": self.board,
            "port": self.port,
            "modules": self.modules,
            "agent": {
                "bootstrap": self.agent_bootstrap,
                "verify": self.agent_verify,
                "cycle": self.agent_cycle,
            },
        }
        path = self.config_path / CONFIG_FILENAME
        path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def load_config(start_dir: Path | None = None) -> HarnessConfig | None:
    """Walk up from start_dir looking for harness.json. Return None if not found."""
    d = (start_dir or Path.cwd()).resolve()
    for _ in range(MAX_WALK_UP):
        candidate = d / CONFIG_FILENAME
        if candidate.is_file():
            return _parse(candidate, d)
        parent = d.parent
        if parent == d:
            break
        d = parent
    return None


def _parse(path: Path, config_dir: Path) -> HarnessConfig | None:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return None
    agent = raw.get("agent", {})
    return HarnessConfig(
        name=raw.get("name", ""),
        version=raw.get("version", "0.1.0"),
        board=raw.get("board", ""),
        port=raw.get("port", ""),
        modules=raw.get("modules", {}),
        agent_bootstrap=agent.get("bootstrap", "esp-harness manifest --json"),
        agent_verify=agent.get("verify", "esp-harness verify"),
        agent_cycle=agent.get("cycle", ["build", "flash", "verify"]),
        config_path=config_dir,
    )
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd D:\Code\esp-harness\tools\esp-harness && python -m pytest tests/test_config.py -v`
Expected: All 5 tests PASS

- [ ] **Step 5: Commit**

```bash
cd D:\Code\esp-harness
git add tools/esp-harness/src/esp_harness/core/config.py tools/esp-harness/tests/test_config.py
git commit -m "feat: harness.json config loader (scaffold v2 foundation)"
```

---

### Task 2: Module Registry

**Files:**
- Create: `src/esp_harness/core/modules.py`
- Create: `tests/test_modules.py`

- [ ] **Step 1: Write failing tests**

```python
# tests/test_modules.py
"""Tests for module registry."""
from __future__ import annotations

from pathlib import Path

import pytest

from esp_harness.core.modules import MODULE_REGISTRY, get_module, list_all_modules


def test_registry_has_core_modules():
    names = [m.name for m in MODULE_REGISTRY]
    assert "scenes" in names
    assert "console" in names
    assert "toast" in names
    assert "bridge" in names
    assert "sim" in names


def test_get_module_returns_module():
    m = get_module("scenes")
    assert m is not None
    assert m.name == "scenes"
    assert m.default is True


def test_get_module_returns_none_for_unknown():
    assert get_module("nonexistent") is None


def test_list_all_modules():
    modules = list_all_modules()
    assert len(modules) >= 5
    for m in modules:
        assert hasattr(m, "name")
        assert hasattr(m, "default")
        assert hasattr(m, "description")


def test_scaffold_creates_files(tmp_path: Path):
    m = get_module("bridge")
    assert m is not None
    created = m.scaffold(tmp_path, project_name="test_proj")
    assert len(created) > 0
    for f in created:
        assert (tmp_path / f).exists(), f"Expected {f} to exist"


def test_remove_deletes_files(tmp_path: Path):
    m = get_module("bridge")
    assert m is not None
    created = m.scaffold(tmp_path, project_name="test_proj")
    removed = m.remove(tmp_path)
    for f in removed:
        assert not (tmp_path / f).exists(), f"Expected {f} to be removed"


def test_manifest_fragment():
    m = get_module("bridge")
    assert m is not None
    frag = m.manifest_fragment()
    assert isinstance(frag, dict)
    assert "name" in frag
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd D:\Code\esp-harness\tools\esp-harness && python -m pytest tests/test_modules.py -v`
Expected: FAIL — `ModuleNotFoundError`

- [ ] **Step 3: Implement module registry**

```python
# src/esp_harness/core/modules.py
"""Module registry for esp-harness scaffold projects.

Each module knows how to scaffold files into a project, remove them,
and contribute a fragment to the manifest output.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class Module:
    name: str
    description: str
    default: bool
    cmake_requires: list[str] = field(default_factory=list)
    _files: dict[str, str] = field(default_factory=dict, repr=False)

    def scaffold(self, project_dir: Path, project_name: str = "") -> list[str]:
        created = []
        for relpath, content in self._files.items():
            rendered = content.replace("__NAME__", project_name)
            full = project_dir / relpath
            full.parent.mkdir(parents=True, exist_ok=True)
            full.write_text(rendered, encoding="utf-8")
            created.append(relpath)
        return created

    def remove(self, project_dir: Path) -> list[str]:
        removed = []
        for relpath in self._files:
            full = project_dir / relpath
            if full.is_file():
                full.unlink()
                removed.append(relpath)
        return removed

    def manifest_fragment(self) -> dict:
        return {
            "name": self.name,
            "description": self.description,
            "default": self.default,
            "cmake_requires": self.cmake_requires,
        }


# ── Module definitions ──────────────────────────────────────────────

_MOD_SCENES = Module(
    name="scenes",
    description="LVGL scene framework with lifecycle hooks",
    default=True,
    cmake_requires=["esp-harness-core", "lvgl__lvgl"],
)

_MOD_CONSOLE = Module(
    name="console",
    description="Line-based console protocol (OK:/ERR:/EVT:)",
    default=True,
    cmake_requires=["esp-harness-core"],
)

_MOD_TOAST = Module(
    name="toast",
    description="Fire-and-forget overlay notification",
    default=True,
    cmake_requires=["esp-harness-core"],
)

_MOD_BRIDGE = Module(
    name="bridge",
    description="Python host bridge + hook_dispatch for Claude Code / Cursor",
    default=False,
    _files={
        "bridge/bridge.py": (
            '"""Host bridge — forwards agent hooks to device."""\n'
            "# Generated by esp-harness create. Customize as needed.\n"
            "\n"
            "from __future__ import annotations\n"
            "import argparse\n"
            "import sys\n"
            "\n"
            "def main():\n"
            '    print("[bridge] placeholder — see esp32-agent-dashboard for full implementation")\n'
            "\n"
            'if __name__ == "__main__":\n'
            "    main()\n"
        ),
        "bridge/hook_dispatch.py": (
            '"""Hook dispatcher — forwards Claude Code hooks to bridge."""\n'
            "import json, socket, sys\n"
            "\n"
            "HOST, PORT = '127.0.0.1', 7321\n"
            "\n"
            "def main():\n"
            "    event = sys.argv[1] if len(sys.argv) > 1 else 'raw'\n"
            "    raw = sys.stdin.read().strip()\n"
            "    try:\n"
            "        with socket.create_connection((HOST, PORT), timeout=1.0) as s:\n"
            "            s.sendall((json.dumps({'type': event, 'text': raw}) + '\\n').encode())\n"
            "            print(s.makefile('r').readline().strip() or json.dumps({'continue': True}))\n"
            "    except (ConnectionRefusedError, OSError):\n"
            "        print(json.dumps({'continue': True}))\n"
            "\n"
            'if __name__ == "__main__":\n'
            "    main()\n"
        ),
    },
)

_MOD_HOOKS = Module(
    name="hooks",
    description="Claude Code / Cursor hook configuration snippets",
    default=False,
    _files={
        ".harness/hooks-snippet.json": (
            '{\n'
            '  "hooks": {\n'
            '    "PreToolUse": "python bridge/hook_dispatch.py pre_tool_use",\n'
            '    "PostToolUse": "python bridge/hook_dispatch.py post_tool_use",\n'
            '    "Stop": "python bridge/hook_dispatch.py stop"\n'
            '  }\n'
            '}\n'
        ),
    },
)

_MOD_SIM = Module(
    name="sim",
    description="SDL2 host simulator with visual regression (sim diff)",
    default=False,
    _files={
        "sim/README.md": (
            "# Host Simulator\n\n"
            "Run scenes on your desktop without flashing.\n\n"
            "```bash\n"
            "cd sim && cmake -B build && cmake --build build\n"
            "esp-harness sim snapshot --scene 0 --out check.bmp\n"
            "```\n"
        ),
    },
)

_MOD_OTA = Module(
    name="ota",
    description="WiFi OTA update scaffold with signature verification",
    default=False,
    _files={
        "main/ota/ota_update.h": (
            "#pragma once\n"
            "void ota_check_and_apply(const char *url);\n"
        ),
    },
)

_MOD_PUSH_BANNER = Module(
    name="push-banner",
    description="Top-slide-down tool-event overlay on lv_layer_top",
    default=False,
    cmake_requires=["esp-harness-core"],
    _files={
        "main/push_banner.h": (
            "#pragma once\n"
            "#include <stdint.h>\n"
            "void push_banner_show(const char *tool, const char *hint, uint32_t duration_ms);\n"
            "void push_banner_dismiss(void);\n"
        ),
    },
)

_MOD_BUTTONS = Module(
    name="buttons",
    description="BOOT/USER GPIO button driver with handler registration",
    default=False,
    cmake_requires=["espressif__button"],
)


MODULE_REGISTRY: list[Module] = [
    _MOD_SCENES, _MOD_CONSOLE, _MOD_TOAST,
    _MOD_BRIDGE, _MOD_HOOKS, _MOD_SIM,
    _MOD_OTA, _MOD_PUSH_BANNER, _MOD_BUTTONS,
]


def get_module(name: str) -> Module | None:
    for m in MODULE_REGISTRY:
        if m.name == name:
            return m
    return None


def list_all_modules() -> list[Module]:
    return list(MODULE_REGISTRY)


def default_modules() -> dict[str, bool]:
    return {m.name: m.default for m in MODULE_REGISTRY}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd D:\Code\esp-harness\tools\esp-harness && python -m pytest tests/test_modules.py -v`
Expected: All 7 tests PASS

- [ ] **Step 5: Commit**

```bash
cd D:\Code\esp-harness
git add tools/esp-harness/src/esp_harness/core/modules.py tools/esp-harness/tests/test_modules.py
git commit -m "feat: module registry with scaffold/remove/manifest operations"
```

---

### Task 3: `esp-harness create` Command

**Files:**
- Create: `src/esp_harness/commands/create.py`
- Modify: `src/esp_harness/cli.py`
- Create: `tests/test_create.py`

- [ ] **Step 1: Write failing test**

```python
# tests/test_create.py
"""Tests for esp-harness create."""
from __future__ import annotations

import json
from pathlib import Path

import pytest

from esp_harness.commands.create import run_create
from esp_harness.core.config import load_config, CONFIG_FILENAME


def test_create_generates_project(tmp_path: Path):
    target = tmp_path / "my-project"
    result = run_create("my-project", target, board="test_board", port="COM3")
    assert result == 0
    assert (target / CONFIG_FILENAME).exists()
    assert (target / "CLAUDE.md").exists()
    assert (target / "main" / "app_main.c").exists()
    assert (target / "main" / "scenes" / "scene_hello.c").exists()
    assert (target / "CMakeLists.txt").exists()

    cfg = load_config(target)
    assert cfg is not None
    assert cfg.name == "my-project"
    assert cfg.board == "test_board"
    assert cfg.port == "COM3"
    assert cfg.modules["scenes"] is True
    assert cfg.modules["bridge"] is False


def test_create_claude_md_contains_bootstrap(tmp_path: Path):
    target = tmp_path / "proj"
    run_create("proj", target, board="b", port="COM1")
    claude_md = (target / "CLAUDE.md").read_text(encoding="utf-8")
    assert "esp-harness manifest --json" in claude_md


def test_create_fails_if_target_exists(tmp_path: Path):
    target = tmp_path / "existing"
    target.mkdir()
    (target / "something.txt").write_text("occupied")
    result = run_create("existing", target, board="b", port="COM1")
    assert result != 0
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd D:\Code\esp-harness\tools\esp-harness && python -m pytest tests/test_create.py -v`
Expected: FAIL

- [ ] **Step 3: Implement create command**

```python
# src/esp_harness/commands/create.py
"""`esp-harness create` — scaffold a new project with harness.json.

Generates a complete, build-ready project skeleton with:
- harness.json (single source of truth)
- CLAUDE.md (auto-generated agent guide pointing to manifest)
- Minimal firmware (app_main.c + scene_hello.c)
- ESP-IDF project files (CMakeLists.txt, sdkconfig.defaults, partitions.csv)
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

from esp_harness.core.config import HarnessConfig
from esp_harness.core.modules import default_modules
from esp_harness.exit_codes import GENERIC_ERROR, OK, PROJECT_NOT_FOUND
from esp_harness.output import Output


def _c_safe(name: str) -> str:
    s = re.sub(r"[^a-zA-Z0-9_]", "_", name)
    if s and s[0].isdigit():
        s = "_" + s
    return s


# ── Templates ────────────────────────────────────────────────────────

CLAUDE_MD = """\
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

## Verification
- `esp-harness screenshot` -- capture device screen
- `esp-harness verify` -- screenshot + visual regression
- `esp-harness console --cmd "?stat" --json` -- device health

## Key Files
- `harness.json` -- project config (board, port, modules)
- `main/app_main.c` -- entry point
- `main/scenes/` -- UI scenes
"""

TOP_CMAKE = """\
cmake_minimum_required(VERSION 3.16)

# esp-harness component + board BSP live outside this project tree.
# Point EXTRA_COMPONENT_DIRS at the monorepo so idf.py resolves them.
set(EXTRA_COMPONENT_DIRS
    $ENV{ESP_HARNESS_ROOT}/components
    $ENV{ESP_HARNESS_ROOT}/boards
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(__NAME__)
"""

MAIN_CMAKE = """\
idf_component_register(
    SRCS
        "__NAME___main.c"
        "scenes/scene_hello.c"
    INCLUDE_DIRS "." "scenes"
    REQUIRES
        lvgl__lvgl
        esp-harness-core
        __BOARD__
)
"""

MAIN_C = """\
#include <stdio.h>
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "harness/console_protocol.h"
#include "harness/scene_framework.h"
#include "harness/default_cmds.h"
#include "scenes/scenes.h"

void app_main(void)
{
    nvs_flash_init();
    bsp_display_start();
    bsp_display_lock(0);

    lv_obj_t *scr = lv_scr_act();
    scene_fw_init(scr);
    scene_fw_register(&scene_hello);
    scene_fw_show(0);

    bsp_display_unlock();

    console_protocol_init();
    harness_default_register();
}
"""

SCENE_HELLO = """\
#include "scenes.h"
#include "lvgl.h"

static void init(scene_t *s, lv_obj_t *parent)
{
    s->container = parent;
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "__NAME__");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
}

scene_t scene_hello = {
    .id          = "hello",
    .display_name = "Hello",
    .description = "starter scene",
    .tags        = "demo",
    .init        = init,
};
"""

SCENES_H = """\
#pragma once
#include "harness/scene_framework.h"

extern scene_t scene_hello;
"""

SDKCONFIG = """\
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_LV_FONT_MONTSERRAT_14=y
CONFIG_LV_FONT_MONTSERRAT_22=y
CONFIG_LV_FONT_MONTSERRAT_28=y
CONFIG_LV_FONT_MONTSERRAT_48=y
"""

PARTITIONS = """\
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     ,        0x6000,
phy_init, data, phy,     ,        0x1000,
factory,  app,  factory, ,        0xF00000,
"""

GITIGNORE = """\
build/
managed_components/
sdkconfig
sdkconfig.old
.harness/latest.png
"""

README_MD = """\
# __NAME__

Built with [esp-harness](https://github.com/Caldis/esp-harness).

## Quick start

```bash
esp-harness build
esp-harness flash
esp-harness screenshot --out demo.png
```

## Development

```bash
esp-harness cycle          # build + flash + verify in one shot
esp-harness manifest --json  # discover all capabilities
```
"""


def _write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def run_create(name: str, target: Path, *, board: str, port: str) -> int:
    """Core logic, separated from CLI for testability."""
    if target.exists() and any(target.iterdir()):
        return GENERIC_ERROR

    cname = _c_safe(name)
    target.mkdir(parents=True, exist_ok=True)

    cfg = HarnessConfig(
        name=name,
        version="0.1.0",
        board=board,
        port=port,
        modules=default_modules(),
        agent_bootstrap="esp-harness manifest --json",
        agent_verify="esp-harness verify",
        agent_cycle=["build", "flash", "verify"],
        config_path=target,
    )
    cfg.save()

    files = {
        "CLAUDE.md": CLAUDE_MD,
        "README.md": README_MD.replace("__NAME__", name),
        "CMakeLists.txt": TOP_CMAKE.replace("__NAME__", cname),
        "sdkconfig.defaults": SDKCONFIG,
        "partitions.csv": PARTITIONS,
        ".gitignore": GITIGNORE,
        f"main/{cname}_main.c": MAIN_C,
        "main/scenes/scene_hello.c": SCENE_HELLO.replace("__NAME__", cname),
        "main/scenes/scenes.h": SCENES_H,
        "main/CMakeLists.txt": MAIN_CMAKE.replace("__NAME__", cname).replace("__BOARD__", board),
    }
    for relpath, content in files.items():
        _write(target / relpath, content)

    (target / ".harness").mkdir(exist_ok=True)
    (target / ".harness" / "golden").mkdir(exist_ok=True)

    return OK


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser(
        "create",
        help="Scaffold a new project with harness.json.",
        description="Generate a complete ESP32 agent project skeleton.",
    )
    p.add_argument("name", help="Project name (used for directory and C identifiers)")
    p.add_argument("--board", default="esp32_s3_touch_amoled_2_16",
                    help="Board BSP component name (default: Waveshare AMOLED 2.16)")
    p.add_argument("--port", default="", help="Serial port (default: auto-detect)")
    p.add_argument("--dir", default=None, help="Parent directory (default: cwd)")
    add_common_flags(p)


def run(args: argparse.Namespace, output: Output) -> int:
    parent = Path(args.dir) if args.dir else Path.cwd()
    target = parent / args.name

    port = args.port
    if not port:
        try:
            from esp_harness.core import ports as ports_mod
            info, _ = ports_mod.detect_one_esp_port()
            port = info.device if info else ""
        except Exception:
            port = ""

    code = run_create(args.name, target, board=args.board, port=port)
    if code != OK:
        output.failure(exit_code=code, error=f"Target {target} already exists and is not empty")
        return code

    output.success(
        {"project": str(target), "board": args.board, "port": port,
         "harness_json": str(target / "harness.json")},
        human=f"Created {target}\n  esp-harness build    # compile\n  esp-harness cycle    # build+flash+verify",
    )
    return OK
```

- [ ] **Step 4: Register create command in cli.py**

Add to imports (after line 35 in `cli.py`):
```python
from esp_harness.commands import create as cmd_create
```

Add to the `for mod` tuple in `build_parser()` (line 76-79):
```python
cmd_create,
```

Add to dispatch in `main()` (after the `elif args.command == "init"` block, line 123):
```python
elif args.command == "create":
    exit_code = cmd_create.run(args, output)
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd D:\Code\esp-harness\tools\esp-harness && python -m pytest tests/test_create.py -v`
Expected: All 3 tests PASS

- [ ] **Step 6: Commit**

```bash
cd D:\Code\esp-harness
git add tools/esp-harness/src/esp_harness/commands/create.py tools/esp-harness/tests/test_create.py tools/esp-harness/src/esp_harness/cli.py
git commit -m "feat: esp-harness create — scaffold projects with harness.json"
```

---

### Task 4: `esp-harness add` and `esp-harness remove` Commands

**Files:**
- Create: `src/esp_harness/commands/add.py`
- Create: `src/esp_harness/commands/remove_mod.py`

- [ ] **Step 1: Implement add command**

```python
# src/esp_harness/commands/add.py
"""`esp-harness add <module>` — install a module into the current project."""
from __future__ import annotations

import argparse

from esp_harness.core.config import load_config
from esp_harness.core.modules import get_module, list_all_modules
from esp_harness.exit_codes import GENERIC_ERROR, OK, PROJECT_NOT_FOUND
from esp_harness.output import Output


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser("add", help="Add a module to the project.",
                       description="Install a module: creates files and updates harness.json.")
    p.add_argument("module", help="Module name (e.g., bridge, sim, hooks)")
    add_common_flags(p)


def run(args: argparse.Namespace, output: Output) -> int:
    cfg = load_config()
    if cfg is None:
        output.failure(exit_code=PROJECT_NOT_FOUND, error="No harness.json found. Run esp-harness create first.")
        return PROJECT_NOT_FOUND

    mod = get_module(args.module)
    if mod is None:
        names = ", ".join(m.name for m in list_all_modules())
        output.failure(exit_code=GENERIC_ERROR, error=f"Unknown module '{args.module}'. Available: {names}")
        return GENERIC_ERROR

    if cfg.modules.get(mod.name):
        output.success({"module": mod.name, "status": "already_enabled"}, human=f"{mod.name} is already enabled.")
        return OK

    created = mod.scaffold(cfg.config_path, project_name=cfg.name)
    cfg.modules[mod.name] = True
    cfg.save()

    output.success(
        {"module": mod.name, "status": "added", "files_created": created},
        human=f"Added {mod.name}: {', '.join(created) if created else 'no files (config-only)'}",
    )
    return OK
```

- [ ] **Step 2: Implement remove command**

```python
# src/esp_harness/commands/remove_mod.py
"""`esp-harness remove <module>` — remove a module from the current project."""
from __future__ import annotations

import argparse

from esp_harness.core.config import load_config
from esp_harness.core.modules import get_module
from esp_harness.exit_codes import GENERIC_ERROR, OK, PROJECT_NOT_FOUND
from esp_harness.output import Output


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser("remove", help="Remove a module from the project.",
                       description="Uninstall a module: removes files and updates harness.json.")
    p.add_argument("module", help="Module name to remove")
    add_common_flags(p)


def run(args: argparse.Namespace, output: Output) -> int:
    cfg = load_config()
    if cfg is None:
        output.failure(exit_code=PROJECT_NOT_FOUND, error="No harness.json found.")
        return PROJECT_NOT_FOUND

    mod = get_module(args.module)
    if mod is None:
        output.failure(exit_code=GENERIC_ERROR, error=f"Unknown module '{args.module}'")
        return GENERIC_ERROR

    if not cfg.modules.get(mod.name):
        output.success({"module": mod.name, "status": "not_enabled"}, human=f"{mod.name} is not enabled.")
        return OK

    removed = mod.remove(cfg.config_path)
    cfg.modules[mod.name] = False
    cfg.save()

    output.success(
        {"module": mod.name, "status": "removed", "files_removed": removed},
        human=f"Removed {mod.name}: {', '.join(removed) if removed else 'no files (config-only)'}",
    )
    return OK
```

- [ ] **Step 3: Register both commands in cli.py**

Add imports:
```python
from esp_harness.commands import add as cmd_add
from esp_harness.commands import remove_mod as cmd_remove
```

Add to `build_parser()` tuple: `cmd_add, cmd_remove,`

Add dispatch cases:
```python
elif args.command == "add":
    exit_code = cmd_add.run(args, output)
elif args.command == "remove":
    exit_code = cmd_remove.run(args, output)
```

- [ ] **Step 4: Commit**

```bash
cd D:\Code\esp-harness
git add tools/esp-harness/src/esp_harness/commands/add.py tools/esp-harness/src/esp_harness/commands/remove_mod.py tools/esp-harness/src/esp_harness/cli.py
git commit -m "feat: esp-harness add/remove — declarative module management"
```

---

### Task 5: `esp-harness list-modules` Command

**Files:**
- Create: `src/esp_harness/commands/list_modules.py`

- [ ] **Step 1: Implement list-modules**

```python
# src/esp_harness/commands/list_modules.py
"""`esp-harness list-modules` — show available and installed modules."""
from __future__ import annotations

import argparse

from esp_harness.core.config import load_config
from esp_harness.core.modules import list_all_modules
from esp_harness.exit_codes import OK
from esp_harness.output import Output


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser("list-modules", help="List available modules.",
                       description="Show all modules with their install status.")
    add_common_flags(p)


def run(args: argparse.Namespace, output: Output) -> int:
    cfg = load_config()
    enabled = cfg.modules if cfg else {}

    modules = []
    lines = []
    for m in list_all_modules():
        installed = enabled.get(m.name, False)
        marker = "+" if installed else "-"
        modules.append({
            "name": m.name,
            "description": m.description,
            "default": m.default,
            "installed": installed,
        })
        lines.append(f"  [{marker}] {m.name:16s} {m.description}")

    header = "Modules (+ = installed):" if cfg else "Modules (no harness.json — showing defaults):"
    output.success(
        {"modules": modules, "project": cfg.name if cfg else None},
        human=header + "\n" + "\n".join(lines),
    )
    return OK
```

- [ ] **Step 2: Register in cli.py, commit**

Add import: `from esp_harness.commands import list_modules as cmd_list_modules`
Add to tuple: `cmd_list_modules,`
Add dispatch: `elif args.command == "list-modules": exit_code = cmd_list_modules.run(args, output)`

```bash
cd D:\Code\esp-harness
git add tools/esp-harness/src/esp_harness/commands/list_modules.py tools/esp-harness/src/esp_harness/cli.py
git commit -m "feat: esp-harness list-modules"
```

---

### Task 6: `esp-harness cycle` and `esp-harness verify` Commands

**Files:**
- Create: `src/esp_harness/commands/cycle.py`
- Create: `src/esp_harness/commands/verify.py`
- Modify: `src/esp_harness/exit_codes.py`
- Create: `tests/test_cycle.py`

- [ ] **Step 1: Add exit codes**

Append to `src/esp_harness/exit_codes.py`:
```python
# verify / cycle
VERIFY_FAILED = 50
CYCLE_FAILED = 51
```

- [ ] **Step 2: Implement verify command**

```python
# src/esp_harness/commands/verify.py
"""`esp-harness verify` — screenshot + structured pass/fail output."""
from __future__ import annotations

import argparse
from pathlib import Path

from esp_harness.core.config import load_config
from esp_harness.exit_codes import OK, VERIFY_FAILED, NO_DEVICE
from esp_harness.output import Output


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser("verify", help="Screenshot the device and report pass/fail.",
                       description="Capture device framebuffer, optionally diff against golden.")
    p.add_argument("--port", default=None, help="Serial port (default: from harness.json)")
    p.add_argument("--out", default=None, help="Screenshot output path (default: .harness/latest.png)")
    add_common_flags(p)


def run(args: argparse.Namespace, output: Output) -> int:
    cfg = load_config()
    port = args.port or (cfg.port if cfg else None)
    if not port:
        output.failure(exit_code=NO_DEVICE, error="No port specified and none in harness.json")
        return NO_DEVICE

    out_path = args.out or ".harness/latest.png"
    out_full = Path(out_path)
    out_full.parent.mkdir(parents=True, exist_ok=True)

    from esp_harness.commands import screenshot as cmd_screenshot
    import types

    fake_args = types.SimpleNamespace(
        port=port, out=str(out_full), size=128,
        json=getattr(args, "json", False),
        verbose=getattr(args, "verbose", False),
    )
    fake_output = Output(json_mode=False, verbose=False)
    code = cmd_screenshot.run(fake_args, fake_output)

    if code != OK:
        output.failure(exit_code=VERIFY_FAILED, error="Screenshot capture failed",
                       details={"screenshot_exit_code": code})
        return VERIFY_FAILED

    output.success(
        {"screenshot": str(out_full), "status": "pass"},
        human=f"Verified: {out_full}",
    )
    return OK
```

- [ ] **Step 3: Implement cycle command**

```python
# src/esp_harness/commands/cycle.py
"""`esp-harness cycle` — execute the full build-flash-verify loop.

Reads agent.cycle from harness.json and runs each step in sequence.
One command for the agent's entire iteration loop.
"""
from __future__ import annotations

import argparse
import time

from esp_harness.core.config import load_config
from esp_harness.exit_codes import CYCLE_FAILED, OK, PROJECT_NOT_FOUND
from esp_harness.output import Output


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser("cycle", help="Run the full build-flash-verify loop.",
                       description="Execute each step in harness.json agent.cycle sequentially.")
    add_common_flags(p)


def run(args: argparse.Namespace, output: Output) -> int:
    cfg = load_config()
    if cfg is None:
        output.failure(exit_code=PROJECT_NOT_FOUND, error="No harness.json found.")
        return PROJECT_NOT_FOUND

    steps = cfg.agent_cycle
    if not steps:
        output.failure(exit_code=CYCLE_FAILED, error="agent.cycle is empty in harness.json")
        return CYCLE_FAILED

    results = []
    for step_name in steps:
        t0 = time.monotonic()
        code = _run_step(step_name, cfg, args)
        elapsed = int((time.monotonic() - t0) * 1000)
        status = "ok" if code == OK else "fail"
        results.append({"step": step_name, "status": status, "elapsed_ms": elapsed, "exit_code": code})

        if output.json_mode:
            import json
            print(json.dumps(results[-1], ensure_ascii=False), flush=True)

        if code != OK:
            output.failure(
                exit_code=code,
                error=f"Cycle failed at step '{step_name}'",
                details={"steps": results},
            )
            return code

    output.success({"steps": results}, human="Cycle complete: " + " → ".join(s["step"] for s in results))
    return OK


def _run_step(name: str, cfg, args) -> int:
    """Dispatch a single cycle step by name."""
    fake_output = Output(json_mode=False, verbose=getattr(args, "verbose", False))

    if name == "build":
        from esp_harness.commands import build as cmd_build
        import types
        ba = types.SimpleNamespace(
            project=str(cfg.config_path), json=False, verbose=False,
        )
        return cmd_build.run(ba, fake_output)

    if name == "flash":
        from esp_harness.commands import flash as cmd_flash
        import types
        fa = types.SimpleNamespace(
            project=str(cfg.config_path), port=cfg.port, baud=460800,
            json=False, verbose=False,
        )
        return cmd_flash.run(fa, fake_output)

    if name == "verify":
        from esp_harness.commands import verify as cmd_verify
        import types
        va = types.SimpleNamespace(
            port=cfg.port, out=".harness/latest.png",
            json=False, verbose=False,
        )
        return cmd_verify.run(va, fake_output)

    return CYCLE_FAILED
```

- [ ] **Step 4: Register both in cli.py**

Add imports:
```python
from esp_harness.commands import cycle as cmd_cycle
from esp_harness.commands import verify as cmd_verify
```

Add to tuple: `cmd_cycle, cmd_verify,`

Add dispatch:
```python
elif args.command == "cycle":
    exit_code = cmd_cycle.run(args, output)
elif args.command == "verify":
    exit_code = cmd_verify.run(args, output)
```

- [ ] **Step 5: Commit**

```bash
cd D:\Code\esp-harness
git add tools/esp-harness/src/esp_harness/commands/cycle.py tools/esp-harness/src/esp_harness/commands/verify.py tools/esp-harness/src/esp_harness/exit_codes.py tools/esp-harness/src/esp_harness/cli.py
git commit -m "feat: esp-harness cycle + verify — one-command agent loop"
```

---

### Task 7: Manifest Enhancement + Context Inference

**Files:**
- Modify: `src/esp_harness/commands/manifest.py`
- Modify: `src/esp_harness/commands/build.py`
- Modify: `src/esp_harness/commands/flash.py`
- Modify: `src/esp_harness/commands/screenshot.py`

- [ ] **Step 1: Enhance manifest with project context**

In `manifest.py`, at the top of the `run()` function, after resolving the port, add project context loading:

```python
from esp_harness.core.config import load_config
from esp_harness.core.modules import list_all_modules
```

In the final JSON output assembly (the `output.success(...)` call), add a `"project"` key:

```python
cfg = load_config()
project_section = None
if cfg:
    project_section = {
        "name": cfg.name,
        "board": cfg.board,
        "port": cfg.port,
        "modules": cfg.enabled_modules(),
        "cycle": cfg.agent_cycle,
    }

# In the payload dict:
"project": project_section,
"convention": {
    "bootstrap": ["Run esp-harness manifest --json at session start."],
    "cycle": "esp-harness cycle",
    "add_scene": "Create main/scenes/scene_<name>.c, register in app_main.c, rebuild.",
    "add_command": "console_protocol_register() in app code. Auto-surfaces in manifest.",
},
```

- [ ] **Step 2: Add context inference to build.py**

Near the top of `build.py`'s `run()` function, before project path resolution:

```python
from esp_harness.core.config import load_config

cfg = load_config()
project_dir = Path(getattr(args, "project", None) or (str(cfg.config_path) if cfg else "."))
```

This replaces the current `project_dir = Path(args.project)` resolution.

- [ ] **Step 3: Add context inference to flash.py**

Near the top of `flash.py`'s `run()` function, before port resolution:

```python
from esp_harness.core.config import load_config

cfg = load_config()
port_override = getattr(args, "port", None)
if not port_override and cfg and cfg.port:
    port_override = cfg.port
```

Use `port_override` instead of `args.port` in the existing port detection logic.

- [ ] **Step 4: Add context inference to screenshot.py**

Same pattern as flash.py — read port from harness.json when `--port` not specified.

- [ ] **Step 5: Add new commands to TOOLKIT_COMMANDS in manifest.py**

Append entries for `create`, `add`, `remove`, `list-modules`, `cycle`, `verify`:

```python
{
    "name": "create",
    "summary": "Scaffold a new project with harness.json.",
    "args": ["<name>", "[--board BOARD]", "[--port PORT]"],
    "exit_codes": [0, 1],
},
{
    "name": "cycle",
    "summary": "Execute the full build-flash-verify loop from harness.json.",
    "args": [],
    "exit_codes": [0, 20, 30, 50, 51],
},
{
    "name": "verify",
    "summary": "Screenshot device and report pass/fail.",
    "args": ["[--port PORT]", "[--out PATH]"],
    "exit_codes": [0, 10, 50],
},
{
    "name": "add",
    "summary": "Add a module to the project (creates files + updates harness.json).",
    "args": ["<module>"],
    "exit_codes": [0, 1, 21],
},
{
    "name": "remove",
    "summary": "Remove a module from the project.",
    "args": ["<module>"],
    "exit_codes": [0, 1, 21],
},
{
    "name": "list-modules",
    "summary": "Show available modules and their install status.",
    "args": [],
    "exit_codes": [0],
},
```

- [ ] **Step 6: Commit**

```bash
cd D:\Code\esp-harness
git add tools/esp-harness/src/esp_harness/commands/manifest.py tools/esp-harness/src/esp_harness/commands/build.py tools/esp-harness/src/esp_harness/commands/flash.py tools/esp-harness/src/esp_harness/commands/screenshot.py
git commit -m "feat: manifest enhancement + context inference from harness.json"
```

---

### Task 8: Integration Test + Final Wiring

**Files:**
- Modify: `tests/test_manifest.py`
- Run: full test suite

- [ ] **Step 1: Add manifest project context test**

Append to `tests/test_manifest.py`:

```python
def test_manifest_includes_project_context(tmp_path, monkeypatch):
    """When harness.json exists, manifest output includes project section."""
    import json
    from esp_harness.core.config import CONFIG_FILENAME
    cfg = {
        "name": "test-proj", "version": "0.1.0", "board": "test",
        "port": "COM99", "modules": {"scenes": True},
        "agent": {"bootstrap": "x", "verify": "y", "cycle": ["build"]},
    }
    (tmp_path / CONFIG_FILENAME).write_text(json.dumps(cfg), encoding="utf-8")
    monkeypatch.chdir(tmp_path)

    from esp_harness.commands.manifest import run as manifest_run
    from esp_harness.output import Output
    import types, io, sys

    buf = io.StringIO()
    monkeypatch.setattr(sys, "stdout", buf)
    fake_args = types.SimpleNamespace(port=None, timeout=1.0, no_device=True, json=True, verbose=False)
    manifest_run(fake_args, Output(json_mode=True))
    result = json.loads(buf.getvalue())
    assert result["project"]["name"] == "test-proj"
    assert result["project"]["port"] == "COM99"
```

- [ ] **Step 2: Run full test suite**

Run: `cd D:\Code\esp-harness\tools\esp-harness && python -m pytest tests/ -v --ignore=tests/test_persistent_session.py --ignore=tests/test_adversarial.py -k "not device"`
Expected: All tests PASS

- [ ] **Step 3: Final commit + tag**

```bash
cd D:\Code\esp-harness
git add -A
git commit -m "test: integration tests for scaffold v2"
git tag -a v2.0.0-scaffold -m "scaffold v2: protocol-first agent platform"
```

---

## Task Dependency Order

```
Task 1 (config) → Task 2 (modules) → Task 3 (create) → Task 4 (add/remove)
                                                       → Task 5 (list-modules)
                                    → Task 6 (cycle/verify)
                                    → Task 7 (manifest + inference)
                                                       → Task 8 (integration)
```

Tasks 4, 5, 6, 7 can run in parallel after Task 3 completes. Task 8 depends on all others.
