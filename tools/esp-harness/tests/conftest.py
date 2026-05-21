"""Pytest fixtures shared across toolkit integration tests.

We don't pull in pytest-embedded — the toolkit already encapsulates
everything the tests need (serial open, OK/ERR parsing, JSON output).
Each test just invokes the CLI as a subprocess and parses the JSON.

That keeps the test harness footprint at zero extra deps (pytest itself
is the only requirement) and ensures the tests exercise the same code
path users hit.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

# In the v1.5 monorepo, paths look like:
#   esp-harness/                      ← MONOREPO_ROOT
#   ├── tools/esp-harness/            ← TOOLKIT_ROOT (this file lives in tests/)
#   │   └── tests/conftest.py
#   └── examples/aurora/              ← AURORA_ROOT (target of "showcase" fixtures)
TOOLKIT_ROOT  = Path(__file__).resolve().parents[1]    # tools/esp-harness/
MONOREPO_ROOT = TOOLKIT_ROOT.parents[1]                # esp-harness/
AURORA_ROOT   = MONOREPO_ROOT / "examples" / "aurora"
VENV_PY       = TOOLKIT_ROOT / ".venv" / "Scripts" / "python.exe"


@pytest.fixture(scope="session")
def esp_harness():
    """Return a callable that runs `esp-harness <args>` and returns
    (returncode, json_dict_or_None, stderr)."""
    if not VENV_PY.exists():
        pytest.skip(f"toolkit venv not present at {VENV_PY}; run install.ps1 first")

    def run(*cli_args, timeout: float = 60.0) -> tuple[int, dict | None, str]:
        cmd = [str(VENV_PY), "-m", "esp_harness", *cli_args, "--json"]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        parsed: dict | None = None
        try:
            # Take the last non-empty line as JSON (some commands print
            # extra status before the result).
            for line in reversed(proc.stdout.splitlines()):
                if line.strip().startswith("{"):
                    parsed = json.loads(line)
                    break
        except Exception:
            parsed = None
        return proc.returncode, parsed, proc.stderr

    return run


@pytest.fixture(scope="session")
def aurora_root() -> Path:
    if not AURORA_ROOT.exists():
        pytest.skip(f"aurora example not at {AURORA_ROOT}")
    return AURORA_ROOT


@pytest.fixture(scope="session")
def sim_binary(aurora_root: Path) -> Path:
    bin_path = aurora_root / "sim" / "build" / "aurora_sim.exe"
    if not bin_path.exists():
        bin_path = aurora_root / "sim" / "build" / "aurora_sim"
    if not bin_path.exists():
        pytest.skip(f"sim binary not built; expected at {bin_path.parent}/aurora_sim[.exe]")
    return bin_path
