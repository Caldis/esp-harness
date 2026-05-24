"""`esp-harness verify` — screenshot + structured pass/fail output."""
from __future__ import annotations

import argparse
from pathlib import Path

from esp_harness.core.config import load_config
from esp_harness.exit_codes import OK, VERIFY_FAILED, NO_DEVICE
from esp_harness.output import Output


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser("verify", help="Screenshot the device and report pass/fail.",
                       description="Capture device framebuffer, optionally diff against golden.")
    p.add_argument("--port", default=None, help="Serial port (default: from harness.json)")
    p.add_argument("--out", default=None, help="Screenshot output path (default: .harness/latest.png)")
    add_common_flags(p)


def run(args: argparse.Namespace, output: Output) -> int:
    cfg = load_config()
    port = args.port or (cfg.port if cfg else None)
    if not port:
        output.failure(exit_code=NO_DEVICE, error="No port specified and none in harness.json")
        return NO_DEVICE

    out_path = args.out or ".harness/latest.png"
    out_full = Path(out_path)
    out_full.parent.mkdir(parents=True, exist_ok=True)

    from esp_harness.commands import screenshot as cmd_screenshot
    import types

    fake_args = types.SimpleNamespace(
        port=port, out=str(out_full), size=128,
        json=getattr(args, "json", False),
        verbose=getattr(args, "verbose", False),
    )
    fake_output = Output(json_mode=False, verbose=False)
    code = cmd_screenshot.run(fake_args, fake_output)

    if code != OK:
        output.failure(exit_code=VERIFY_FAILED, error="Screenshot capture failed",
                       details={"screenshot_exit_code": code})
        return VERIFY_FAILED

    output.success(
        {"screenshot": str(out_full), "status": "pass"},
        human=f"Verified: {out_full}",
    )
    return OK
