"""`esp-harness test` — run the toolkit integration suite.

Thin wrapper around `pytest tools/tests`. Exists so users don't have
to remember the venv interpreter path or the test directory location.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from esp_harness.exit_codes import GENERIC_ERROR, OK
from esp_harness.output import Output


# v1.5 monorepo: this file lives at tools/esp-harness/src/esp_harness/commands/test.py.
# parents[3] is tools/esp-harness/ (the toolkit package root); tests/ lives
# directly under it (not under a nested tools/, that was the v1.4 layout).
TOOLKIT_ROOT = Path(__file__).resolve().parents[3]
TESTS_DIR = TOOLKIT_ROOT / "tests"


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser(
        "test",
        help="Run the toolkit integration test suite (pytest tools/tests).",
        description=(
            "Discovers tools/tests/test_*.py and runs them via pytest. "
            "Tests subprocess-invoke `esp-harness <cmd>` so they exercise "
            "the same code path as users hit."
        ),
    )
    p.add_argument("-k", dest="keyword", default=None,
                   help="Only run tests matching this keyword (pytest -k).")
    p.add_argument("--verbose-pytest", action="store_true",
                   help="Pass -v to pytest for per-test output.")
    add_common_flags(p)


def run(args: argparse.Namespace, output: Output) -> int:
    if not TESTS_DIR.exists():
        output.failure(exit_code=GENERIC_ERROR,
                       error=f"no tests dir at {TESTS_DIR}")
        return GENERIC_ERROR

    cmd = [sys.executable, "-m", "pytest", str(TESTS_DIR)]
    if args.verbose_pytest:
        cmd.append("-v")
    if args.keyword:
        cmd += ["-k", args.keyword]

    output.info(f"running: {' '.join(cmd)}")
    try:
        proc = subprocess.run(cmd, timeout=600)
    except subprocess.TimeoutExpired:
        output.failure(exit_code=GENERIC_ERROR, error="pytest timed out")
        return GENERIC_ERROR

    if proc.returncode != 0:
        output.failure(
            exit_code=GENERIC_ERROR,
            error=f"pytest exited {proc.returncode}",
            details={"tests_dir": str(TESTS_DIR)},
        )
        return GENERIC_ERROR
    output.success(
        {"tests_dir": str(TESTS_DIR), "result": "all passed"},
        human=f"pytest passed in {TESTS_DIR}",
    )
    return OK
