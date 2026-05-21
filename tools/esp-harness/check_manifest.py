"""Lint: every registered CLI subcommand must appear in
TOOLKIT_COMMANDS (see commands/manifest.py).

Why: the manifest is the AI-discoverable surface. If a developer adds a
new subcommand to cli.py but forgets to list it, the next session can't
find it. CI runs this script to break the build instead of silently
shipping a half-described capability.

Usage:
    python tools/check_manifest.py        # exit 0 if everything lines up
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "src"))

from esp_harness import cli                                # noqa: E402
from esp_harness.commands.manifest import TOOLKIT_COMMANDS  # noqa: E402


def cli_subcommand_names() -> set[str]:
    """Build the parser argparse builds and read every subparser name."""
    parser = cli.build_parser()
    for action in parser._actions:
        if hasattr(action, "choices") and action.choices:
            return set(action.choices.keys())
    return set()


def manifest_names() -> set[str]:
    return {c["name"] for c in TOOLKIT_COMMANDS}


def main() -> int:
    cli_set = cli_subcommand_names()
    man_set = manifest_names()

    missing_in_manifest = cli_set - man_set
    missing_in_cli = man_set - cli_set

    if not missing_in_manifest and not missing_in_cli:
        print(f"OK: {len(cli_set)} CLI subcommands all present in manifest.")
        return 0

    if missing_in_manifest:
        print("FAIL: CLI subcommands without a manifest entry:")
        for n in sorted(missing_in_manifest):
            print(f"  - {n}  (add to TOOLKIT_COMMANDS in commands/manifest.py)")
    if missing_in_cli:
        print("FAIL: manifest entries with no corresponding CLI subcommand:")
        for n in sorted(missing_in_cli):
            print(f"  - {n}  (remove from TOOLKIT_COMMANDS or wire up in cli.py)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
