"""Aurora-specific pytest fixtures.

These used to live in `tools/esp-harness/tests/conftest.py` — wrong
place, because they're consumer-level fixtures (they assume Aurora's
sim binary exists, that Aurora's source tree is at `examples/aurora/`,
etc.). Per the G-6 resolution, framework tests must be framework-only;
consumer tests live with their consumer's tree.

Tests in this directory (`examples/aurora/tests/`) get framework
fixtures from the top-level conftest hierarchy when they run via the
toolkit venv with `pytest examples/aurora/tests/`. Run with::

    cd D:/Code/esp-harness/examples/aurora
    ../../tools/esp-harness/.venv/Scripts/python.exe -m pytest tests/ -q

or from the framework root with::

    ./tools/esp-harness/.venv/Scripts/python.exe -m pytest examples/aurora/tests/ -q

The framework's own pytest run (`pytest tools/esp-harness/tests/`)
must NOT touch this directory.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

EXAMPLE_ROOT = Path(__file__).resolve().parents[1]      # examples/aurora/
MONOREPO_ROOT = EXAMPLE_ROOT.parents[1]                 # esp-harness/
TOOLKIT_ROOT  = MONOREPO_ROOT / "tools" / "esp-harness"
VENV_PY       = TOOLKIT_ROOT / ".venv" / "Scripts" / "python.exe"


@pytest.fixture(scope="session")
def esp_harness():
    """Mirror of the framework's CLI runner, scoped to Aurora's test
    needs. Same signature as the upstream one so framework tests can
    move into this tree without rewriting."""
    if not VENV_PY.exists():
        pytest.skip(f"toolkit venv not present at {VENV_PY}")

    def run(*cli_args, timeout: float = 60.0) -> tuple[int, dict | None, str]:
        cmd = [str(VENV_PY), "-m", "esp_harness", *cli_args, "--json"]
        proc = subprocess.run(
            cmd, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=timeout,
        )
        parsed: dict | None = None
        try:
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
    """Aurora's project root. Always available in this test tree."""
    return EXAMPLE_ROOT


@pytest.fixture(scope="session")
def sim_binary(aurora_root: Path) -> Path:
    """The compiled aurora_sim binary. Skips if not built — running
    Aurora's sim suite requires Aurora's sim to be built first."""
    bin_path = aurora_root / "sim" / "build" / "aurora_sim.exe"
    if not bin_path.exists():
        bin_path = aurora_root / "sim" / "build" / "aurora_sim"
    if not bin_path.exists():
        pytest.skip(f"sim binary not built; expected at {bin_path.parent}/aurora_sim[.exe]")
    return bin_path
