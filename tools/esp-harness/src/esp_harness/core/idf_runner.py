"""Invoke ESP-IDF tooling from a clean subprocess.

ESP-IDF lives in an isolated Python venv installed by EIM. The venv's Scripts
dir is NOT on the global PATH. To call `idf.py` we:

1. Once per process, spawn `powershell` and dot-source the EIM-generated
   profile (which sets IDF_PATH, IDF_TOOLS_PATH, PATH, etc.), then dump the
   resulting environment as `KEY=value` lines. Cache it.
2. For each `idf.py` invocation, build a Popen env dict from os.environ plus
   the cached IDF env, and exec the venv's python directly against
   `<IDF_PATH>/tools/idf.py`. We bypass the PowerShell function `idf.py`
   alias because subprocess can't call PS functions.

This module is environment-discovery + a single `run_idf(...)` helper.
Commands compose on top.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path
from typing import Iterable

# Default EIM-generated profile (matches v6.0.1 install). If a future version
# changes the path, expose ESP_HARNESS_EIM_PROFILE env var to override.
DEFAULT_EIM_PROFILE = Path(r"C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1")

_cached_env: dict[str, str] | None = None


class EnvError(RuntimeError):
    """Raised when the ESP-IDF environment cannot be established."""


def _find_eim_profile() -> Path:
    override = os.environ.get("ESP_HARNESS_EIM_PROFILE")
    if override:
        p = Path(override)
        if not p.exists():
            raise EnvError(f"ESP_HARNESS_EIM_PROFILE points to missing file: {p}")
        return p
    if DEFAULT_EIM_PROFILE.exists():
        return DEFAULT_EIM_PROFILE
    # try to discover any EIM profile under C:\Espressif\tools\
    candidates = sorted(Path(r"C:\Espressif\tools").glob("Microsoft.v*.PowerShell_profile.ps1"))
    if candidates:
        return candidates[-1]  # latest version
    raise EnvError(
        "No EIM profile found. Looked at "
        f"{DEFAULT_EIM_PROFILE} and C:\\Espressif\\tools\\Microsoft.v*.PowerShell_profile.ps1. "
        "Re-run EIM installer or set ESP_HARNESS_EIM_PROFILE."
    )


def _get_idf_env() -> dict[str, str]:
    """Lazily activate EIM env in a subprocess, capture & cache resulting env vars."""
    global _cached_env
    if _cached_env is not None:
        return _cached_env

    profile = _find_eim_profile()
    script = (
        f". '{profile}' *>$null; "
        "Get-ChildItem env: | ForEach-Object { \"$($_.Name)=$($_.Value)\" }"
    )
    try:
        result = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
            capture_output=True,
            text=True,
            timeout=30,
        )
    except FileNotFoundError as e:
        raise EnvError("'powershell' not found on PATH") from e
    except subprocess.TimeoutExpired as e:
        raise EnvError("EIM activation timed out after 30s") from e

    if result.returncode != 0:
        raise EnvError(f"EIM activation failed (exit {result.returncode}):\n{result.stderr}")

    env: dict[str, str] = {}
    for line in result.stdout.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            k = k.strip()
            v = v.strip()
            if k:
                env[k] = v

    # sanity check
    if "IDF_PATH" not in env or "IDF_PYTHON_ENV_PATH" not in env:
        raise EnvError(
            f"EIM activation didn't yield IDF_PATH/IDF_PYTHON_ENV_PATH. "
            f"Got keys: {sorted(env.keys())[:20]}…"
        )

    _cached_env = env
    return env


def get_python_exe() -> Path:
    """Path to the ESP-IDF venv's python.exe."""
    env = _get_idf_env()
    venv = Path(env["IDF_PYTHON_ENV_PATH"])
    return venv / "Scripts" / "python.exe"


def get_idf_py() -> Path:
    """Path to <IDF_PATH>/tools/idf.py."""
    env = _get_idf_env()
    return Path(env["IDF_PATH"]) / "tools" / "idf.py"


def get_idf_version() -> str:
    """Reported by EIM profile (e.g. '6.0')."""
    return _get_idf_env().get("ESP_IDF_VERSION", "?")


def build_subprocess_env() -> dict[str, str]:
    """Merge current os.environ with IDF env (IDF wins where keys collide)."""
    merged = os.environ.copy()
    merged.update(_get_idf_env())
    return merged


def run_idf(
    args: Iterable[str],
    *,
    project_dir: Path,
    capture: bool = True,
    timeout: float | None = None,
    extra_env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    """Run `idf.py <args>` in `project_dir`. Returns CompletedProcess (text mode).

    `capture=True` collects stdout/stderr into the result; `capture=False`
    streams to the parent's stdout/stderr (useful for live builds).
    """
    python_exe = get_python_exe()
    idf_py = get_idf_py()
    env = build_subprocess_env()
    if extra_env:
        env.update(extra_env)

    cmd = [str(python_exe), str(idf_py), *list(args)]
    return subprocess.run(
        cmd,
        cwd=str(project_dir),
        env=env,
        text=True,
        capture_output=capture,
        timeout=timeout,
        check=False,
    )


def run_idf_streaming(
    args: Iterable[str],
    *,
    project_dir: Path,
    on_line=None,  # callable(str) -> None
    timeout: float | None = None,
    extra_env: dict[str, str] | None = None,
) -> tuple[int, list[str]]:
    """Like `run_idf` but streams stdout line-by-line. Returns (returncode, all_lines)."""
    python_exe = get_python_exe()
    idf_py = get_idf_py()
    env = build_subprocess_env()
    if extra_env:
        env.update(extra_env)

    cmd = [str(python_exe), str(idf_py), *list(args)]
    proc = subprocess.Popen(
        cmd,
        cwd=str(project_dir),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=1,
    )
    lines: list[str] = []
    assert proc.stdout is not None
    try:
        for raw in proc.stdout:
            line = raw.rstrip("\r\n")
            lines.append(line)
            if on_line is not None:
                on_line(line)
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        raise
    return proc.returncode, lines
