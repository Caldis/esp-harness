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
