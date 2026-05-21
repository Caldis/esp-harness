"""`esp-harness run` — build + flash + capture in one shot.

The AI's "one iteration" command. Composes the other primitives and returns
a unified JSON envelope so the agent can decide next action from a single call.

Phases:
    1. build   (skip with --no-build)
    2. flash
    3. capture (--seconds, optional --until regex)

If any phase fails, subsequent phases are skipped and the failure exit code
is returned. The JSON payload always reports what phases ran and their results.
"""

from __future__ import annotations

import argparse
import time
from pathlib import Path

from esp_harness.commands import build as cmd_build
from esp_harness.core import backtrace as bt
from esp_harness.core import idf_runner
from esp_harness.core import ports as ports_mod
from esp_harness.core import serial_io
from esp_harness.exit_codes import (
    AMBIGUOUS_DEVICE,
    BUILD_FAILED,
    DEVICE_BUSY,
    FLASH_FAILED,
    MONITOR_TIMEOUT,
    NO_DEVICE,
    OK,
    PROJECT_NOT_FOUND,
)
from esp_harness.output import Output


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser(
        "run",
        help="build + flash + monitor one shot (the AI iteration command).",
    )
    p.add_argument("--project", type=Path, default=Path.cwd())
    p.add_argument("--port", default=None)
    p.add_argument("--flash-baud", type=int, default=460800)
    p.add_argument("--monitor-baud", type=int, default=115200)
    p.add_argument("--seconds", type=float, default=8.0,
                   help="Capture duration after flash (default 8s).")
    p.add_argument("--until", default=None,
                   help="Stop capture once this regex matches in the output.")
    p.add_argument("--no-build", action="store_true",
                   help="Skip the build step (use existing artifacts).")
    p.add_argument("--require-match", action="store_true",
                   help="Fail MONITOR_TIMEOUT if --until is given and never matches.")
    p.add_argument("--quiet", action="store_true")
    add_common_flags(p)


def _resolve_port(requested: str | None, output: Output) -> tuple[str | None, int]:
    if requested:
        return requested, OK
    chosen, candidates = ports_mod.detect_one_esp_port()
    if chosen is not None:
        return chosen.port, OK
    if not candidates:
        return None, NO_DEVICE
    return None, AMBIGUOUS_DEVICE


def run(args: argparse.Namespace, output: Output) -> int:
    project = Path(args.project).resolve()
    if not (project / "CMakeLists.txt").is_file():
        output.failure(
            exit_code=PROJECT_NOT_FOUND,
            error=f"No CMakeLists.txt in {project}",
        )
        return PROJECT_NOT_FOUND

    live_print = (not args.quiet) and (not output.json_mode)
    phases: dict[str, dict] = {}
    overall_started = time.monotonic()

    # ── phase 1: build ──────────────────────────────────────────────
    if not args.no_build:
        output.info("[1/3] build")
        started = time.monotonic()
        captured: list[str] = []

        def on_line(line: str) -> None:
            captured.append(line)
            if live_print:
                print(line)

        try:
            rc, lines = idf_runner.run_idf_streaming(
                ["build"], project_dir=project, on_line=on_line
            )
        except idf_runner.EnvError as e:
            output.failure(exit_code=100, error=str(e))
            return 100

        build_ms = int((time.monotonic() - started) * 1000)
        errors, warnings = cmd_build._parse_errors(lines)  # type: ignore[attr-defined]
        artifacts = cmd_build._find_artifacts(project)  # type: ignore[attr-defined]
        phases["build"] = {
            "ok": rc == 0,
            "elapsed_ms": build_ms,
            "returncode": rc,
            "n_errors": len(errors),
            "n_warnings": len(warnings),
            "errors": errors if rc != 0 else [],
            "warnings": warnings,
            "artifacts": artifacts,
        }
        if rc != 0:
            output.failure(
                exit_code=BUILD_FAILED,
                error=f"build failed (exit {rc})",
                details={"phases": phases, "total_elapsed_ms": int((time.monotonic() - overall_started) * 1000)},
            )
            return BUILD_FAILED
    else:
        phases["build"] = {"ok": True, "skipped": True}

    # ── resolve port for flash + monitor ────────────────────────────
    port, code = _resolve_port(args.port, output)
    if port is None:
        msg = "No ESP32 port" if code == NO_DEVICE else "Ambiguous ESP32 ports"
        output.failure(
            exit_code=code,
            error=msg,
            details={"phases": phases},
        )
        return code

    # ── phase 2: flash ──────────────────────────────────────────────
    output.info(f"[2/3] flash → {port} @ {args.flash_baud}")
    started = time.monotonic()
    flash_lines: list[str] = []

    def on_flash_line(line: str) -> None:
        flash_lines.append(line)
        if live_print:
            print(line)

    try:
        rc, _ = idf_runner.run_idf_streaming(
            ["-p", port, "-b", str(args.flash_baud), "flash"],
            project_dir=project,
            on_line=on_flash_line,
        )
    except idf_runner.EnvError as e:
        output.failure(exit_code=100, error=str(e))
        return 100

    flash_ms = int((time.monotonic() - started) * 1000)
    phases["flash"] = {
        "ok": rc == 0,
        "port": port,
        "baud": args.flash_baud,
        "elapsed_ms": flash_ms,
        "returncode": rc,
    }
    if rc != 0:
        full = "\n".join(flash_lines)
        exit_code = DEVICE_BUSY if ("access" in full.lower() or "permission" in full.lower()) else FLASH_FAILED
        phases["flash"]["tail"] = flash_lines[-30:]
        output.failure(
            exit_code=exit_code,
            error=f"flash failed (exit {rc})",
            details={"phases": phases, "total_elapsed_ms": int((time.monotonic() - overall_started) * 1000)},
        )
        return exit_code

    # ── phase 3: monitor (capture) ──────────────────────────────────
    output.info(f"[3/3] capture {args.seconds}s @ {args.monitor_baud}")
    started = time.monotonic()
    try:
        capture_res = serial_io.capture(
            port=port,
            baud=args.monitor_baud,
            seconds=args.seconds,
            until=args.until,
            on_line=(lambda line: print(line)) if live_print else None,
        )
    except Exception as e:
        phases["monitor"] = {"ok": False, "error": str(e)}
        output.failure(
            exit_code=MONITOR_TIMEOUT,
            error=f"capture failed: {e}",
            details={"phases": phases},
        )
        return MONITOR_TIMEOUT
    capture_ms = int((time.monotonic() - started) * 1000)

    # Backtrace auto-decode against this project's ELF.
    decoded = bt.decoded_to_jsonable(
        bt.decode_text(capture_res.text, project_dir=project)
    )

    phases["monitor"] = {
        "ok": True,
        "port": port,
        "baud": args.monitor_baud,
        "elapsed_ms": capture_ms,
        "matched": capture_res.matched_pattern,
        "timed_out": capture_res.timed_out,
        "n_lines": len(capture_res.lines),
        "text": capture_res.text,
        "lines": capture_res.lines,
        "decoded_backtrace": decoded,
    }

    if args.require_match and args.until and not capture_res.matched_pattern:
        output.failure(
            exit_code=MONITOR_TIMEOUT,
            error=f"capture timed out after {args.seconds}s without matching /{args.until}/",
            details={"phases": phases},
        )
        return MONITOR_TIMEOUT

    total_ms = int((time.monotonic() - overall_started) * 1000)
    output.success(
        {
            "project": str(project),
            "total_elapsed_ms": total_ms,
            "phases": phases,
        },
        human=f"\nrun OK: build {phases['build'].get('elapsed_ms','-')}ms, "
              f"flash {phases['flash']['elapsed_ms']}ms, "
              f"capture {phases['monitor']['elapsed_ms']}ms "
              f"({phases['monitor']['n_lines']} lines)",
    )
    return OK
