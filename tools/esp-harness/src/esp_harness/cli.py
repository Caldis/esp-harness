"""Top-level CLI dispatcher.

    esp-harness <command> [...]

Every subcommand module exposes:
    add_subparser(subparsers) -> None
    run(args, output) -> int  (exit code)
"""

from __future__ import annotations

import argparse
import os
import sys
import traceback

# Force UTF-8 on Windows so unicode arrows / Chinese device names don't
# render as mojibake (`?ping �� OK` etc.). PowerShell on a fresh user
# session defaults to a legacy CP (cp936 / cp1252 / similar); the user
# shouldn't have to know about chcp 65001 to get readable output.
if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    # Best-effort: ask cmd.exe / PowerShell to switch its code page.
    # Failure is non-fatal — reconfigure above already covers most cases.
    try:
        os.system("chcp 65001 > NUL 2>&1")
    except Exception:
        pass

from esp_harness import __version__
from esp_harness.commands import adversarial as cmd_adversarial
from esp_harness.commands import audio as cmd_audio
from esp_harness.commands import backtrace as cmd_backtrace
from esp_harness.commands import bench as cmd_bench
from esp_harness.commands import build as cmd_build
from esp_harness.commands import console as cmd_console
from esp_harness.commands import doctor as cmd_doctor
from esp_harness.commands import manifest as cmd_manifest
from esp_harness.commands import flash as cmd_flash
from esp_harness.commands import init as cmd_init   # v1.4 compat alias for `new`
from esp_harness.commands import monitor as cmd_monitor
from esp_harness.commands import new as cmd_new
from esp_harness.commands import port as cmd_port
from esp_harness.commands import run as cmd_run
from esp_harness.commands import screenshot as cmd_screenshot
from esp_harness.commands import sim as cmd_sim
from esp_harness.commands import tap as cmd_tap
from esp_harness.commands import test as cmd_test
from esp_harness.exit_codes import CLI_MISUSE, GENERIC_ERROR
from esp_harness.output import Output


def _add_common_flags(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--json", action="store_true", help="Emit a single JSON object to stdout."
    )
    parser.add_argument(
        "--verbose", "-v", action="store_true", help="Show diagnostic logs on stderr."
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="esp-harness",
        description="Agent-friendly ESP-IDF dev harness (build / flash / monitor / run).",
    )
    parser.add_argument("--version", action="version", version=f"esp-harness {__version__}")

    sub = parser.add_subparsers(dest="command", metavar="<command>")
    sub.required = True

    for mod in (cmd_port, cmd_build, cmd_flash, cmd_monitor, cmd_run, cmd_tap,
                cmd_screenshot, cmd_audio, cmd_console, cmd_manifest,
                cmd_backtrace, cmd_bench, cmd_sim, cmd_new, cmd_init,
                cmd_doctor, cmd_test, cmd_adversarial):
        mod.add_subparser(sub, _add_common_flags)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    output = Output(json_mode=getattr(args, "json", False), verbose=getattr(args, "verbose", False))

    try:
        # Dispatch
        if args.command == "port":
            exit_code = cmd_port.run(args, output)
        elif args.command == "build":
            exit_code = cmd_build.run(args, output)
        elif args.command == "flash":
            exit_code = cmd_flash.run(args, output)
        elif args.command == "monitor":
            exit_code = cmd_monitor.run(args, output)
        elif args.command == "run":
            exit_code = cmd_run.run(args, output)
        elif args.command == "tap":
            exit_code = cmd_tap.run(args, output)
        elif args.command == "screenshot":
            exit_code = cmd_screenshot.run(args, output)
        elif args.command == "audio":
            exit_code = cmd_audio.run(args, output)
        elif args.command == "console":
            exit_code = cmd_console.run(args, output)
        elif args.command == "manifest":
            exit_code = cmd_manifest.run(args, output)
        elif args.command == "backtrace":
            exit_code = cmd_backtrace.run(args, output)
        elif args.command == "bench":
            exit_code = cmd_bench.run(args, output)
        elif args.command == "sim":
            exit_code = cmd_sim.run(args, output)
        elif args.command == "new":
            exit_code = cmd_new.run(args, output)
        elif args.command == "init":
            # v1.4 alias: forward to `new` with the same args (back-compat).
            exit_code = cmd_init.run(args, output)
        elif args.command == "doctor":
            exit_code = cmd_doctor.run(args, output)
        elif args.command == "test":
            exit_code = cmd_test.run(args, output)
        elif args.command == "adversarial":
            exit_code = cmd_adversarial.run(args, output)
        else:
            parser.print_help(sys.stderr)
            exit_code = CLI_MISUSE
    except KeyboardInterrupt:
        output.failure(exit_code=GENERIC_ERROR, error="Interrupted by user")
        exit_code = GENERIC_ERROR
    except SystemExit:
        raise
    except Exception as e:
        details: dict[str, object] = {"exception_type": type(e).__name__}
        if output.verbose:
            details["traceback"] = traceback.format_exc()
        output.failure(exit_code=GENERIC_ERROR, error=str(e), details=details)
        exit_code = GENERIC_ERROR

    sys.exit(exit_code)


if __name__ == "__main__":
    main()
