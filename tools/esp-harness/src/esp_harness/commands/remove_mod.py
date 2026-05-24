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
