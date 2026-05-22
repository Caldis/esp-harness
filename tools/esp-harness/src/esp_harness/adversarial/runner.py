"""Runner — coordinates persona dispatch, aggregation, and convergence loop.

The runner is the place where "abstract persona registry" meets
"actual AI subagent call". This v0 ships with a *dry-run dispatcher*
that simply emits the prompts that WOULD be sent, plus a *manual
dispatcher* that reads pre-baked findings from a JSON file (useful
for testing the aggregator without an AI session). Real-AI dispatch
plugs in via the ``dispatcher`` callable.

The convergence loop runs `--rounds` rounds (or `--until-converged`),
re-feeding the prior round's findings into the next round's personas
so falsifications stack. The loop exits when:

  - critical count is 0 (the user-configurable definition of
    converged), OR
  - --rounds budget exhausted, OR
  - --budget tokens exhausted (tracked across rounds).

Output: a `findings.json` per round + a `summary.json` at the end.
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict
from pathlib import Path
from typing import Callable, Optional

from esp_harness.adversarial import (
    Finding,
    PersonaContext,
    all_personas,
    get_persona,
)
from esp_harness.adversarial.aggregator import (
    dedupe,
    severity_sort,
    summarise,
)


# ──────────────────────────────────────────────────────────────────
# Built-in dispatchers
# ──────────────────────────────────────────────────────────────────

PersonaDispatcher = Callable[[str, PersonaContext], list[Finding]]


def dry_run_dispatcher(persona_name: str, ctx: PersonaContext) -> list[Finding]:
    """Print the prompt the persona would send. Returns no findings.

    Useful for plumbing tests and for human eyeballs to review the
    prompt template before plugging in a real AI dispatcher.
    """
    p = get_persona(persona_name)
    if p is None:
        return []
    prompt = p.prompt_fn(ctx)
    sep = "─" * 60
    print(f"\n{sep}\n[dry-run] persona={persona_name}\n{sep}")
    print(prompt)
    print(sep)
    return []


def manual_dispatcher_factory(findings_jsonl_path: Path) -> PersonaDispatcher:
    """Read pre-baked findings from a JSONL file. One JSON Finding per
    line. Useful for testing the aggregator deterministically."""
    def _disp(persona_name: str, ctx: PersonaContext) -> list[Finding]:
        if not findings_jsonl_path.exists():
            return []
        out: list[Finding] = []
        for line in findings_jsonl_path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            d = json.loads(line)
            if d.get("persona") != persona_name:
                continue
            out.append(Finding(
                id=d["id"], persona=d["persona"], severity=d["severity"],
                what_broke=d["what_broke"], evidence=d.get("evidence", {}),
                code_location=d["code_location"],
                suggested_fix=d.get("suggested_fix", ""),
                cross_check_persona=d.get("cross_check_persona"),
                smoke_case_proposal=d.get("smoke_case_proposal"),
                confidence=d.get("confidence", 0.5),
            ))
        return out
    return _disp


# ──────────────────────────────────────────────────────────────────
# Run one cycle / one full session
# ──────────────────────────────────────────────────────────────────


def run_one_round(
    ctx: PersonaContext,
    persona_names: list[str],
    dispatcher: PersonaDispatcher,
    round_n: int,
) -> list[Finding]:
    """Spawn each persona in sequence (parallel is dispatcher's job
    if it wants), collect findings, attach round_n."""
    raw: list[Finding] = []
    for name in persona_names:
        findings = dispatcher(name, ctx)
        for f in findings:
            f.round_n = round_n
        raw.extend(findings)
    return raw


def run_session(
    project_root: str,
    persona_names: list[str],
    *,
    rounds: int = 1,
    until_converged: bool = False,
    dispatcher: Optional[PersonaDispatcher] = None,
    findings_out: Optional[Path] = None,
    smoke_command: Optional[str] = None,
) -> dict:
    """Run a full adversarial session and return the final summary."""
    dispatcher = dispatcher or dry_run_dispatcher
    started = time.monotonic()

    accumulated: list[Finding] = []
    if findings_out is not None:
        findings_out.mkdir(parents=True, exist_ok=True)

    for round_n in range(1, rounds + 1):
        ctx = PersonaContext(
            project_root=project_root,
            smoke_command=smoke_command,
            prior_findings=accumulated,
        )
        round_findings = run_one_round(ctx, persona_names, dispatcher, round_n)
        accumulated.extend(round_findings)
        accumulated = dedupe(accumulated)
        accumulated = severity_sort(accumulated)

        if findings_out is not None:
            out_path = findings_out / f"round-{round_n:02d}.json"
            out_path.write_text(
                json.dumps([asdict(f) for f in accumulated], indent=2),
                encoding="utf-8",
            )

        if until_converged:
            critical = sum(1 for f in accumulated if f.severity == "critical")
            if critical == 0:
                break

    summary = summarise(accumulated)
    summary["elapsed_s"] = round(time.monotonic() - started, 2)
    summary["rounds_run"] = round_n
    summary["persona_names"] = persona_names

    if findings_out is not None:
        (findings_out / "summary.json").write_text(
            json.dumps(summary, indent=2), encoding="utf-8"
        )

    return summary
