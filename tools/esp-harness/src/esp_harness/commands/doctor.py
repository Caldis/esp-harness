"""`esp-harness doctor` — environment health check.

Probes every external dependency the toolkit + sim build chain expects
and prints a status report. JSON mode is the AI contract; human mode is
the colorised report.

Exit codes:
  0  everything required is present
  1  at least one REQUIRED check failed (toolkit can't function)
  2  only OPTIONAL checks failed (some workflows degraded)

We don't auto-install anything — installing into the user's environment
is a system change that requires their explicit consent. Each failing
check prints the install command instead.
"""

from __future__ import annotations

import argparse
import importlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

from esp_harness.exit_codes import GENERIC_ERROR, OK
from esp_harness.output import Output


def _which_or_eim(exe_name: str, eim_glob: str | None = None) -> tuple[str | None, str | None]:
    """Return (path, version_string) or (None, None). Checks PATH first,
    then a glob under C:\\Espressif\\tools (the EIM install root)."""
    p = shutil.which(exe_name)
    if not p and eim_glob:
        for cand in Path("C:/Espressif/tools").glob(eim_glob):
            if cand.is_file():
                p = str(cand)
                break
    if not p:
        return None, None
    try:
        out = subprocess.run([p, "--version"], capture_output=True, text=True, timeout=5)
        v = (out.stdout + out.stderr).splitlines()[0].strip() if (out.stdout or out.stderr) else ""
        return p, v
    except Exception:
        return p, ""


def _check_cmake() -> dict:
    p, v = _which_or_eim("cmake", "cmake/*/bin/cmake.exe")
    if p:
        return {"name": "cmake", "status": "ok", "path": p, "version": v, "required": True}
    return {
        "name": "cmake", "status": "missing", "required": True,
        "hint": "Install via: scoop install cmake   OR   choco install cmake -y   OR   use the EIM install at C:\\Espressif\\tools\\cmake\\<ver>\\bin\\",
    }


def _check_idf() -> dict:
    """Detect ESP-IDF. Preference order:
        1. EIM-installed PowerShell profile under C:\\Espressif\\tools\\ (Win)
        2. IDF_PATH environment variable (any platform, manual install)
        3. ~/esp/esp-idf/ or ~/esp-idf/ (Unix conventional location)
    """
    # 1. EIM profile (Windows, preferred — toolkit uses this directly).
    activate = list(Path("C:/Espressif/tools").glob("Microsoft.v*.PowerShell_profile.ps1"))
    if activate:
        latest = sorted(activate)[-1]
        ver = latest.stem.split(".PowerShell")[0].lstrip("Microsoft.v")
        return {"name": "esp-idf", "status": "ok",
                "path": str(latest), "version": ver, "required": True,
                "via": "EIM"}
    # 2. IDF_PATH env var (any platform).
    idf_path = os.environ.get("IDF_PATH")
    if idf_path and (Path(idf_path) / "tools" / "idf.py").is_file():
        ver = "?"
        version_file = Path(idf_path) / "version.txt"
        if version_file.exists():
            try:
                ver = version_file.read_text().strip().lstrip("v")
            except Exception:
                pass
        return {"name": "esp-idf", "status": "ok",
                "path": idf_path, "version": ver, "required": True,
                "via": "IDF_PATH",
                "note": "toolkit auto-activation is Windows/EIM only; "
                        "on other platforms run `. $IDF_PATH/export.sh` first"}
    # 3. Conventional Unix install locations.
    for candidate in [Path.home() / "esp" / "esp-idf",
                      Path.home() / "esp-idf"]:
        if (candidate / "tools" / "idf.py").is_file():
            return {"name": "esp-idf", "status": "warn",
                    "path": str(candidate), "required": True,
                    "note": "found at conventional path but IDF_PATH not "
                            "exported — set it or `. export.sh` first"}
    return {
        "name": "esp-idf", "status": "missing", "required": True,
        "hint": "Install via EIM (Win): https://docs.espressif.com/projects/idf-im-ui/  "
                "OR install manually then `export IDF_PATH=~/esp/esp-idf`",
    }


def _check_python_pkg(pkg_name: str, required: bool, hint: str) -> dict:
    try:
        mod = importlib.import_module(pkg_name)
        v = getattr(mod, "__version__", "?")
        return {"name": f"py:{pkg_name}", "status": "ok",
                "version": v, "required": required}
    except ImportError:
        return {"name": f"py:{pkg_name}", "status": "missing",
                "required": required, "hint": hint}


def _check_mingw() -> dict:
    p, v = _which_or_eim("gcc")
    if not p:
        # Try scoop path
        scoop_gcc = Path.home() / "scoop" / "apps" / "mingw" / "current" / "bin" / "gcc.exe"
        if scoop_gcc.exists():
            p = str(scoop_gcc)
            try:
                out = subprocess.run([p, "--version"], capture_output=True, text=True, timeout=5)
                v = out.stdout.splitlines()[0]
            except Exception:
                v = ""
    if p:
        return {"name": "mingw-gcc", "status": "ok", "path": p,
                "version": v, "required": False,
                "note": "needed for sim/ host build only"}
    return {
        "name": "mingw-gcc", "status": "missing", "required": False,
        "note": "only needed for the desktop sim/ build",
        "hint": "scoop install mingw   OR   choco install mingw -y   (only needed for sim)",
    }


def _check_sdl2() -> dict:
    candidates = [
        Path.home() / "scoop" / "apps" / "sdl2",
        Path("C:/SDL2"),
        Path("C:/ProgramData/chocolatey/lib/sdl2"),
        Path(os.environ.get("SDL2_DIR", "")) if os.environ.get("SDL2_DIR") else None,
    ]
    for c in candidates:
        if c and c.exists():
            # Look for the actual header (SDL.h) inside
            for sub in c.rglob("SDL.h"):
                return {"name": "sdl2", "status": "ok", "path": str(sub.parent),
                        "required": False, "note": "needed for sim/ host build only"}
    return {
        "name": "sdl2", "status": "missing", "required": False,
        "note": "only needed for the desktop sim/ build",
        "hint": (
            "Win: download SDL2-devel-*-mingw.zip from "
            "https://github.com/libsdl-org/SDL/releases and extract to "
            "%USERPROFILE%\\scoop\\apps\\sdl2 (or set SDL2_DIR).  "
            "Linux/Mac: apt install libsdl2-dev | brew install sdl2.  "
            "See examples/aurora/sim/INTEGRATION.md for full setup."
        ),
    }


def _check_serial_port() -> dict:
    try:
        from esp_harness.core import ports as ports_mod
        chosen, candidates = ports_mod.detect_one_esp_port()
        if chosen:
            return {"name": "esp32-port", "status": "ok",
                    "path": chosen.port, "version": chosen.description,
                    "required": False, "note": "device-connected workflows only"}
        if candidates:
            return {"name": "esp32-port", "status": "warn",
                    "required": False,
                    "note": f"{len(candidates)} ambiguous candidate(s); pass --port explicitly",
                    "candidates": [c.port for c in candidates]}
        return {"name": "esp32-port", "status": "missing",
                "required": False,
                "note": "no ESP32 device connected; sim/ workflows still work"}
    except Exception as e:
        return {"name": "esp32-port", "status": "missing",
                "required": False, "note": f"probe failed: {e}"}


def _check_harness_component() -> dict:
    """Locate components/aurora-harness/. In the v1.5 monorepo it's a
    peer at <root>/components/aurora-harness/; legacy layouts had it as
    a sibling repo (esp32-harness-showcase/components/aurora-harness)."""
    toolkit_root  = Path(__file__).resolve().parents[3]    # tools/esp-harness/
    monorepo_root = toolkit_root.parents[1]                # esp-harness/

    # v1.5 monorepo first
    monorepo_comp = monorepo_root / "components" / "aurora-harness"
    if monorepo_comp.exists():
        return {"name": "aurora-harness", "status": "ok",
                "path": str(monorepo_comp),
                "required": False,
                "note": f"monorepo layout — auto-detect from {monorepo_root}"}

    # Legacy v1.3 sibling layout
    legacy_sibling = toolkit_root.parent / "esp32-harness-showcase" / "components" / "aurora-harness"
    if legacy_sibling.exists():
        return {"name": "aurora-harness", "status": "ok",
                "path": str(legacy_sibling),
                "required": False,
                "note": "legacy sibling layout (pre v1.5)"}

    return {"name": "aurora-harness", "status": "warn",
            "required": False,
            "note": "aurora-harness component not found — pass --bin / --project explicitly to sim / init",
            "hint": f"git clone https://github.com/Caldis/esp-harness {monorepo_root.parent}"}


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser(
        "doctor",
        help="Probe toolkit dependencies and report their status.",
        description=(
            "Walks through every external dep (ESP-IDF, cmake, Pillow, "
            "MinGW gcc, SDL2 dev libs, serial port, sibling showcase repo) "
            "and reports what's healthy / missing / warning. JSON mode is "
            "the AI contract; human mode is colourised."
        ),
    )
    add_common_flags(p)


def run(args: argparse.Namespace, output: Output) -> int:
    checks: list[dict] = [
        _check_idf(),
        _check_cmake(),
        _check_python_pkg("PIL", required=True,
                          hint="pip install Pillow"),
        _check_python_pkg("serial", required=True,
                          hint="pip install pyserial   (probably broken venv — re-run install.ps1)"),
        _check_mingw(),
        _check_sdl2(),
        _check_serial_port(),
        _check_harness_component(),
    ]

    # Human-mode rendering
    if not output.json_mode:
        sym = {"ok": "[OK  ]", "warn": "[WARN]", "missing": "[MISS]"}
        for c in checks:
            tag = "REQ" if c.get("required") else "opt"
            line = f"  {sym.get(c['status'], '[?]')} ({tag}) {c['name']:<14}"
            if c.get("version"):
                line += f" {c['version']}"
            if c.get("path"):
                line += f"  ({c['path']})"
            print(line)
            if c.get("note"):
                print(f"          note:  {c['note']}")
            if c["status"] != "ok" and c.get("hint"):
                print(f"          hint:  {c['hint']}")
        print()

    failed_required = [c for c in checks if c["status"] == "missing" and c.get("required")]
    failed_optional = [c for c in checks if c["status"] == "missing" and not c.get("required")]
    warn = [c for c in checks if c["status"] == "warn"]
    n_ok = sum(1 for c in checks if c["status"] == "ok")

    overall = {
        "ok": len(failed_required) == 0,
        "n_checks": len(checks),
        "n_ok": n_ok,
        "n_warn": len(warn),
        "n_missing_required": len(failed_required),
        "n_missing_optional": len(failed_optional),
        "checks": checks,
    }

    if failed_required:
        output.failure(
            exit_code=GENERIC_ERROR,
            error=f"{len(failed_required)} required check(s) failed: "
                  + ", ".join(c["name"] for c in failed_required),
            details=overall,
        )
        return GENERIC_ERROR
    output.success(
        overall,
        human=(
            f"{n_ok}/{len(checks)} OK · "
            f"{len(warn)} warn · {len(failed_optional)} optional missing"
        ),
    )
    return OK
