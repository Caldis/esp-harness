"""`esp-harness init <name>` — deprecated v1.4 alias for `esp-harness new`.

Previous behaviour: scaffolded a project whose CMakeLists.txt defaulted
`AURORA_HARNESS_DIR` to `../esp32-harness-showcase/components` — a path
that hasn't existed since the v1.5 monorepo flip. Round-4 adversarial
subagent caught that the deprecated alias produced an unbuildable
project on a fresh install.

Current behaviour: forwards to `new` semantics with a one-line
deprecation note on stderr. The output project is the v1.7-era
link-mode scaffold (BSP auto-wired against the monorepo), so
`esp-harness init my-thing && cd my-thing && esp-harness build`
actually works.

Will be removed in v2.0.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from esp_harness.commands import new as cmd_new
from esp_harness.output import Output


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser(
        "init",
        help="(deprecated alias for `new`) Scaffold a starter ESP-IDF + LVGL project.",
        description=(
            "Deprecated since v1.5 — kept for muscle memory. Forwards to "
            "`esp-harness new` (link-mode default, BSP auto-wired). Use "
            "`esp-harness new <name>` directly in new scripts."
        ),
    )
    p.add_argument("name", help="Project directory to create. Must not exist yet.")
    # Old --harness-dir flag is accepted for backward compatibility but
    # rerouted to new's --harness-root (which points at the monorepo
    # ROOT, not the components/ subdir as the old alias did).
    p.add_argument(
        "--harness-dir",
        dest="harness_root_legacy",
        type=Path,
        default=None,
        help="(deprecated) Path to the directory holding aurora-harness/. "
             "Pre-v1.5 this defaulted to ../esp32-harness-showcase/components. "
             "Today: prefer `--harness-root` on `esp-harness new`.",
    )
    add_common_flags(p)


def run(args: argparse.Namespace, output: Output) -> int:
    output.warn(
        "`esp-harness init` is deprecated since v1.5 — forwarding to "
        "`esp-harness new --component-source link`. Use `esp-harness new` "
        "directly going forward."
    )
    # Synthesise a Namespace that `new.run` accepts. The legacy
    # --harness-dir pointed at the components/ subdir; new's
    # --harness-root expects the monorepo root one level up. If the
    # caller passed --harness-dir, strip the trailing /components.
    harness_root = None
    if getattr(args, "harness_root_legacy", None):
        legacy = Path(args.harness_root_legacy)
        if legacy.name == "components":
            harness_root = legacy.parent
        else:
            harness_root = legacy

    new_args = argparse.Namespace(
        name=args.name,
        component_source="link",
        harness_root=harness_root,
        json=getattr(args, "json", False),
        verbose=getattr(args, "verbose", False),
    )
    return cmd_new.run(new_args, output)
