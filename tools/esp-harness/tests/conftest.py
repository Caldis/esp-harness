"""Pytest fixtures for the FRAMEWORK's own integration tests.

After the G-6 cleanup (May 2026), this conftest is **framework-only**.
The Aurora-specific fixtures (`aurora_root`, `sim_binary`) and the
test that needed them (`test_sim_diff.py`) moved to
`examples/aurora/tests/`. Framework pytest must remain project-
agnostic — it probes the TOOLKIT's own surface (doctor, manifest,
parser, payload reader, persistent session, adversarial), not any
consumer's content.

To run Aurora's consumer tests::

    pytest examples/aurora/tests/ -q
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

TOOLKIT_ROOT  = Path(__file__).resolve().parents[1]    # tools/esp-harness/
MONOREPO_ROOT = TOOLKIT_ROOT.parents[1]                # esp-harness/
VENV_PY       = TOOLKIT_ROOT / ".venv" / "Scripts" / "python.exe"


@pytest.fixture(scope="session")
def esp_harness():
    """Return a callable that runs `esp-harness <args>` and returns
    (returncode, json_dict_or_None, stderr)."""
    if not VENV_PY.exists():
        pytest.skip(f"toolkit venv not present at {VENV_PY}; run install.ps1 first")

    def run(*cli_args, timeout: float = 60.0) -> tuple[int, dict | None, str]:
        cmd = [str(VENV_PY), "-m", "esp_harness", *cli_args, "--json"]
        # The CLI emits UTF-8; pin the subprocess decode to match. On a
        # Windows locale (cp936 / gbk) the default `text=True` decode
        # crashes when the CLI prints Chinese device descriptions or
        # other non-ASCII content.
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
        )
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


# Aurora-specific fixtures moved to examples/aurora/tests/conftest.py
# (May 2026, G-6 fix). Framework fixtures stay project-agnostic.
