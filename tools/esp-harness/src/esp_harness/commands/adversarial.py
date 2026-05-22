"""`esp-harness adversarial` — the v1.8 north-star command.

Runs N adversarial personas against a consumer project, collects
findings, dedupes / cross-checks them, and reports convergence.
See ``esp_harness.adversarial`` for the persona registry + runner
internals.

Quick usage::

    # dry-run: show what prompts would be sent
    esp-harness adversarial --personas verify,falsify --rounds 1 \\
        --project . --dry-run

    # with a manual findings fixture (for testing the aggregator)
    esp-harness adversarial --personas verify,falsify --rounds 2 \\
        --project . --manual-findings ./fixtures/findings.jsonl \\
        --findings-out ./out/

    # list registered personas
    esp-harness adversarial --list-personas
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from esp_harness.adversarial import all_personas, get_persona
from esp_harness.adversarial.runner import (
    dry_run_dispatcher,
    manual_dispatcher_factory,
    run_session,
)
from esp_harness.exit_codes import CLI_MISUSE, GENERIC_ERROR, OK
from esp_harness.output import Output


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser(
        "adversarial",
        help="run an adversarial multi-persona falsification round",
        description=__doc__,
    )
    add_common_flags(p)
    p.add_argument(
        "--personas", default="verify,falsify",
        help="comma-separated persona names (default: verify,falsify; "
             "see --list-personas for the full registry).",
    )
    p.add_argument(
        "--rounds", type=int, default=1,
        help="how many adversarial rounds to run (default: 1).",
    )
    p.add_argument(
        "--until-converged", action="store_true",
        help="loop until critical-count is 0 or --rounds exhausted.",
    )
    p.add_argument(
        "--project", default=".",
        help="root of the consumer project to attack (default: cwd).",
    )
    p.add_argument(
        "--smoke-command", default=None,
        help="smoke gate command (e.g. 'pwsh tools/smoke.ps1'); "
             "personas use it as a context anchor.",
    )
    p.add_argument(
        "--findings-out", default=None,
        help="directory to write per-round findings JSON + summary.",
    )
    p.add_argument(
        "--list-personas", action="store_true",
        help="print the registered personas and exit.",
    )
    p.add_argument(
        "--dry-run", action="store_true",
        help="print the prompts personas would send; don't actually "
             "spawn AI subagents (v0 default while real-AI dispatcher "
             "lands).",
    )
    p.add_argument(
        "--manual-findings", default=None,
        help="path to a JSONL file of pre-baked Finding objects; "
             "use for aggregator testing without an AI session.",
    )


def run(args, output: Output) -> int:
    if args.list_personas:
        for p in all_personas():
            output.info(f"{p.name:12} {p.description}")
        return OK

    persona_names = [n.strip() for n in args.personas.split(",") if n.strip()]
    for n in persona_names:
        if get_persona(n) is None:
            output.warn(f"unknown persona '{n}'; see --list-personas")
            return CLI_MISUSE

    # Choose dispatcher
    if args.manual_findings:
        dispatcher = manual_dispatcher_factory(Path(args.manual_findings))
    elif args.dry_run:
        dispatcher = dry_run_dispatcher
    else:
        # v0 only ships dry-run; real-AI dispatcher is v0.3.1+.
        output.info("[adversarial] no AI dispatcher available in v0 — "
                    "defaulting to --dry-run mode.")
        dispatcher = dry_run_dispatcher

    findings_out = Path(args.findings_out) if args.findings_out else None
    summary = run_session(
        project_root=args.project,
        persona_names=persona_names,
        rounds=args.rounds,
        until_converged=args.until_converged,
        dispatcher=dispatcher,
        findings_out=findings_out,
        smoke_command=args.smoke_command,
    )

    if args.json:
        output.success(summary)
        return OK if summary.get("converged", False) else 50
    else:
        output.info(f"[adversarial] {summary['total']} findings "
                    f"across {summary['rounds_run']} round(s)")
        for sev, n in summary["by_severity"].items():
            output.info(f"  {sev:14s} {n}")
        if summary.get("critical_first"):
            output.info("\nCritical:")
            for f in summary["critical_first"]:
                output.info(f"  - {f['what_broke']}  ({f['where']})")
        output.info(f"\nconverged={summary['converged']}  "
                    f"elapsed={summary['elapsed_s']}s")

    return OK if summary.get("converged", False) else 50  # non-zero on critical
