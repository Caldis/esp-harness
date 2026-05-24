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
