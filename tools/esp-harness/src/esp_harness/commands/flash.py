"""`esp-harness flash` — wrap `idf.py -p <port> flash` with structured output."""

from __future__ import annotations

import argparse
import re
import time
from pathlib import Path

from esp_harness.core import idf_runner
from esp_harness.core import ports as ports_mod
from esp_harness.exit_codes import (
    AMBIGUOUS_DEVICE,
    DEVICE_BUSY,
    FLASH_FAILED,
    NO_DEVICE,
    OK,
    PROJECT_NOT_FOUND,
)
from esp_harness.output import Output


_WROTE_RE = re.compile(r"Wrote\s+(\d+)\s+bytes", re.IGNORECASE)
_HASH_OK_RE = re.compile(r"Hash of data verified", re.IGNORECASE)
_PORT_BUSY_RE = re.compile(r"(could not open port|PermissionError|access is denied)", re.IGNORECASE)


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser("flash", help="Flash firmware (wraps `idf.py -p <port> flash`).")
    p.add_argument("--project", type=Path, default=Path.cwd(),
                   help="ESP-IDF project path (default: cwd).")
    p.add_argument("--port", default=None,
                   help="COM port. If omitted, auto-detect.")
    p.add_argument("--baud", type=int, default=460800,
                   help="Baud rate for flashing (default: 460800).")
    p.add_argument("--quiet", action="store_true", help="Suppress live output.")
    add_common_flags(p)


def _resolve_port(requested: str | None, output: Output) -> tuple[str | None, int]:
    """Return (port_or_None, exit_code). If port is None, exit_code is non-zero."""
    if requested:
        return requested, OK
    chosen, candidates = ports_mod.detect_one_esp_port()
    if chosen is not None:
        output.debug(f"auto-detected port {chosen.port} ({chosen.chip_guess})")
        return chosen.port, OK
    if not candidates:
        output.failure(
            exit_code=NO_DEVICE,
            error="No ESP32 port found. Plug in the board or pass --port.",
        )
        return None, NO_DEVICE
    output.failure(
        exit_code=AMBIGUOUS_DEVICE,
        error=f"Ambiguous: {len(candidates)} ESP32 candidates. Pass --port.",
        details={"candidates": [c.to_dict() for c in candidates]},
    )
    return None, AMBIGUOUS_DEVICE


def run(args: argparse.Namespace, output: Output) -> int:
    project = Path(args.project).resolve()
    if not (project / "CMakeLists.txt").is_file():
        output.failure(
            exit_code=PROJECT_NOT_FOUND,
            error=f"No CMakeLists.txt in {project}",
        )
        return PROJECT_NOT_FOUND

    port, code = _resolve_port(args.port, output)
    if port is None:
        return code

    output.info(f"flashing {project} → {port} @ {args.baud}")

    live_print = (not args.quiet) and (not output.json_mode)
    started = time.monotonic()
    captured: list[str] = []

    def on_line(line: str) -> None:
        captured.append(line)
        if live_print:
            print(line)

    try:
        returncode, all_lines = idf_runner.run_idf_streaming(
            ["-p", port, "-b", str(args.baud), "flash"],
            project_dir=project,
            on_line=on_line,
        )
    except idf_runner.EnvError as e:
        output.failure(exit_code=100, error=str(e))
        return 100

    elapsed_ms = int((time.monotonic() - started) * 1000)
    full_text = "\n".join(all_lines)
    wrote_bytes = sum(int(m.group(1)) for m in _WROTE_RE.finditer(full_text))
    verified = bool(_HASH_OK_RE.search(full_text))

    if returncode == 0:
        output.success(
            {
                "elapsed_ms": elapsed_ms,
                "project": str(project),
                "port": port,
                "baud": args.baud,
                "wrote_bytes": wrote_bytes,
                "verified": verified,
            },
            human=f"flashed {wrote_bytes:,} bytes to {port} in {elapsed_ms/1000:.1f}s "
                  f"(verified: {verified})",
        )
        return OK

    if _PORT_BUSY_RE.search(full_text):
        output.failure(
            exit_code=DEVICE_BUSY,
            error=f"Port {port} busy or access denied.",
            details={"port": port, "elapsed_ms": elapsed_ms, "returncode": returncode},
            human="Close any other program holding the port (VS Code Serial Monitor, "
                  "PuTTY, etc.).",
        )
        return DEVICE_BUSY

    output.failure(
        exit_code=FLASH_FAILED,
        error=f"flash failed (exit {returncode}) after {elapsed_ms/1000:.1f}s",
        details={
            "port": port,
            "baud": args.baud,
            "elapsed_ms": elapsed_ms,
            "returncode": returncode,
            "tail": all_lines[-30:],
        },
    )
    return FLASH_FAILED
