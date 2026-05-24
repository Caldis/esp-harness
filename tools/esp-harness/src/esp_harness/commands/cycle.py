"""`esp-harness cycle` — execute the full build-flash-verify loop.

Reads agent.cycle from harness.json and runs each step in sequence.
One command for the agent's entire iteration loop.
"""
from __future__ import annotations

import argparse
import time

from esp_harness.core.config import load_config
from esp_harness.exit_codes import CYCLE_FAILED, OK, PROJECT_NOT_FOUND
from esp_harness.output import Output


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser("cycle", help="Run the full build-flash-verify loop.",
                       description="Execute each step in harness.json agent.cycle sequentially.")
    add_common_flags(p)


def run(args: argparse.Namespace, output: Output) -> int:
    cfg = load_config()
    if cfg is None:
        output.failure(exit_code=PROJECT_NOT_FOUND, error="No harness.json found.")
        return PROJECT_NOT_FOUND

    steps = cfg.agent_cycle
    if not steps:
        output.failure(exit_code=CYCLE_FAILED, error="agent.cycle is empty in harness.json")
        return CYCLE_FAILED

    results = []
    for step_name in steps:
        t0 = time.monotonic()
        code = _run_step(step_name, cfg, args)
        elapsed = int((time.monotonic() - t0) * 1000)
        status = "ok" if code == OK else "fail"
        results.append({"step": step_name, "status": status, "elapsed_ms": elapsed, "exit_code": code})

        if output.json_mode:
            import json
            print(json.dumps(results[-1], ensure_ascii=False), flush=True)

        if code != OK:
            output.failure(
                exit_code=code,
                error=f"Cycle failed at step '{step_name}'",
                details={"steps": results},
            )
            return code

    output.success({"steps": results}, human="Cycle complete: " + " → ".join(s["step"] for s in results))
    return OK


def _run_step(name: str, cfg, args) -> int:
    """Dispatch a single cycle step by name."""
    fake_output = Output(json_mode=False, verbose=getattr(args, "verbose", False))

    if name == "build":
        from esp_harness.commands import build as cmd_build
        import types
        ba = types.SimpleNamespace(
            project=str(cfg.config_path), json=False, verbose=False,
        )
        return cmd_build.run(ba, fake_output)

    if name == "flash":
        from esp_harness.commands import flash as cmd_flash
        import types
        fa = types.SimpleNamespace(
            project=str(cfg.config_path), port=cfg.port, baud=460800,
            json=False, verbose=False,
        )
        return cmd_flash.run(fa, fake_output)

    if name == "verify":
        from esp_harness.commands import verify as cmd_verify
        import types
        va = types.SimpleNamespace(
            port=cfg.port, out=".harness/latest.png",
            json=False, verbose=False,
        )
        return cmd_verify.run(va, fake_output)

    return CYCLE_FAILED
