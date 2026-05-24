"""`esp-harness build` — wrap `idf.py build` with structured output.

We stream idf.py's stdout/stderr live (so humans see progress), accumulate
all lines, then parse for errors/warnings on exit. Exit code 0 implies
success; non-zero implies BUILD_FAILED with extracted error lines.
"""

from __future__ import annotations

import argparse
import re
import time
from pathlib import Path

from esp_harness.core import idf_runner, patches
from esp_harness.core.config import load_config
from esp_harness.exit_codes import BUILD_FAILED, OK, PROJECT_NOT_FOUND
from esp_harness.output import Output

# gcc/clang error: "/path/file.c:42:10: error: 'foo' undeclared"
_GCC_RE = re.compile(r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?:(?P<col>\d+):)?\s*(?P<level>error|fatal error|warning):\s*(?P<msg>.*)$")
# ninja: error / FAILED: lines
_NINJA_FAILED_RE = re.compile(r"^(?:FAILED:|ninja: error:)\s*(?P<msg>.*)$")
# ld.lld errors
_LD_RE = re.compile(r"^ld\.lld:\s+error:\s+(?P<msg>.*)$")


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser("build", help="Build the project (wraps `idf.py build`).")
    p.add_argument(
        "--project",
        type=Path,
        default=Path.cwd(),
        help="Path to the ESP-IDF project (default: cwd).",
    )
    p.add_argument(
        "--quiet",
        action="store_true",
        help="Suppress live build output on stderr (still captured for JSON).",
    )
    add_common_flags(p)


def _parse_errors(lines: list[str]) -> tuple[list[dict], list[dict]]:
    errors: list[dict] = []
    warnings: list[dict] = []
    for line in lines:
        m = _GCC_RE.match(line.strip())
        if m:
            entry = {
                "file": m["file"],
                "line": int(m["line"]),
                "col": int(m["col"]) if m["col"] else None,
                "level": m["level"],
                "message": m["msg"],
                "raw": line,
            }
            (errors if "error" in m["level"] else warnings).append(entry)
            continue
        m = _NINJA_FAILED_RE.match(line.strip())
        if m:
            errors.append({"level": "build", "message": m["msg"], "raw": line})
            continue
        m = _LD_RE.match(line.strip())
        if m:
            errors.append({"level": "link", "message": m["msg"], "raw": line})
    return errors, warnings


def _find_artifacts(project_dir: Path) -> dict[str, str]:
    build_dir = project_dir / "build"
    artifacts: dict[str, str] = {}
    if build_dir.is_dir():
        for ext, key in [("*.elf", "elf"), ("*.bin", "bin"), ("*.map", "map")]:
            matches = sorted(build_dir.glob(ext))
            if matches:
                artifacts[key] = str(matches[0].resolve())
    return artifacts


def run(args: argparse.Namespace, output: Output) -> int:
    cfg = load_config()
    # If --project was not explicitly provided (still the default cwd),
    # prefer the harness.json config_path as the project root.
    raw_project = args.project
    if cfg and raw_project == Path.cwd():
        project = cfg.config_path.resolve()
    else:
        project = Path(raw_project).resolve()
    if not (project / "CMakeLists.txt").is_file():
        output.failure(
            exit_code=PROJECT_NOT_FOUND,
            error=f"No CMakeLists.txt in {project}",
            details={"project": str(project)},
            human="Pass --project <path> or cd into the project root.",
        )
        return PROJECT_NOT_FOUND

    output.info(f"building project: {project}")

    started = time.monotonic()
    live_print = (not args.quiet) and (not output.json_mode)

    def on_line(line: str) -> None:
        if live_print:
            print(line)

    # Pre-build: apply known managed_components patches if managed_components
    # already exists from a previous reconfigure (idempotent).
    pre_patches = patches.apply_all(project)
    applied_pre = [p["name"] for p in pre_patches if p["applied"]]
    if applied_pre:
        output.info(f"applied pre-build patches: {', '.join(applied_pre)}")

    try:
        returncode, all_lines = idf_runner.run_idf_streaming(
            ["build"], project_dir=project, on_line=on_line
        )
    except idf_runner.EnvError as e:
        output.failure(exit_code=100, error=str(e))
        return 100

    # If we hit a known upstream issue (e.g. qmi8658 needing the i2c_master
    # split-out fix), apply patches and retry once. The first build typically
    # populates managed_components/ before failing — after the patches land,
    # the second pass succeeds.
    if returncode != 0:
        stderr_blob = "\n".join(all_lines)
        if patches.stderr_suggests_retry(stderr_blob):
            retry_results = patches.apply_all(project)
            applied = [p["name"] for p in retry_results if p["applied"]]
            if applied:
                output.info(f"applied post-failure patches: {', '.join(applied)}, retrying build")
                try:
                    returncode, all_lines = idf_runner.run_idf_streaming(
                        ["build"], project_dir=project, on_line=on_line
                    )
                except idf_runner.EnvError as e:
                    output.failure(exit_code=100, error=str(e))
                    return 100

    elapsed_ms = int((time.monotonic() - started) * 1000)

    errors, warnings = _parse_errors(all_lines)
    artifacts = _find_artifacts(project)

    # Round-3 adversarial subagent caught this: from Git Bash on Windows
    # idf.py prints `MSys/Mingw is no longer supported. Please follow
    # the getting started guide...` and exits 0 *without compiling
    # anything*. build.py used to accept rc=0 as success and return
    # whatever stale ELF/BIN was already on disk — an AI agent who
    # called `flash` after such a build would flash old code with no
    # warning. Treat the message as a hard failure regardless of rc.
    msys_refusal = any(
        "MSys/Mingw is no longer supported" in line for line in all_lines
    )
    if msys_refusal:
        output.failure(
            exit_code=100,
            error=("idf.py refused to build inside MSys/Mingw "
                   "(common with Git Bash on Windows)."),
            details={
                "elapsed_ms": elapsed_ms,
                "project": str(project),
                "returncode": returncode,
                "trigger": "MSys/Mingw is no longer supported",
            },
            human=("Re-run from PowerShell (not Git Bash) so idf.py's "
                   "MSys check doesn't short-circuit. The toolkit's "
                   "internal idf_runner activates the EIM env directly "
                   "and is independent of your shell, but it can't "
                   "override idf.py's own MSys detection."),
        )
        return 100

    # Sanity gate against `returncode == 0 but nothing actually built`:
    # if no ELF newer than this invocation's start, treat as failure.
    # Catches any other future "warning but exit 0" cases we don't yet
    # know about.
    if returncode == 0 and artifacts.get("elf"):
        try:
            import os as _os
            elf_mtime = _os.path.getmtime(artifacts["elf"])
            if elf_mtime < started - 5.0:  # 5s slack for clock skew
                output.failure(
                    exit_code=BUILD_FAILED,
                    error=("build returned 0 but no new artifact produced "
                           "(ELF mtime predates build start by "
                           f"{started - elf_mtime:.0f}s)."),
                    details={
                        "elapsed_ms": elapsed_ms,
                        "project": str(project),
                        "elf": artifacts["elf"],
                        "elf_age_seconds": int(started - elf_mtime),
                    },
                    human="The toolchain ran but produced no output. "
                          "Likely an environment-detection short-circuit. "
                          "Try running `idf.py build` directly to see "
                          "the underlying refusal.",
                )
                return BUILD_FAILED
        except OSError:
            pass

    if returncode == 0:
        payload = {
            "elapsed_ms": elapsed_ms,
            "project": str(project),
            "warnings": warnings,
            "artifacts": artifacts,
            "n_warnings": len(warnings),
        }
        output.success(payload, human=f"build OK in {elapsed_ms/1000:.1f}s "
                                       f"({len(warnings)} warnings)")
        return OK

    output.failure(
        exit_code=BUILD_FAILED,
        error=f"build failed (exit {returncode}) after {elapsed_ms/1000:.1f}s",
        details={
            "elapsed_ms": elapsed_ms,
            "project": str(project),
            "returncode": returncode,
            "errors": errors,
            "warnings": warnings,
            "n_errors": len(errors),
            "n_warnings": len(warnings),
        },
        human="\n".join(
            f"  {e.get('file','?')}:{e.get('line','?')}: {e.get('level','error')}: {e['message']}"
            for e in errors[:10]
        ) if errors else None,
    )
    return BUILD_FAILED
