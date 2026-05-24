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
