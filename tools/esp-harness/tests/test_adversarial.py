"""Smoke tests for the adversarial primitive (v0.3.0).

Covers the plumbing: persona registry self-registers, dry-run
dispatcher prints prompts without dispatching, manual dispatcher
reads JSONL fixtures, aggregator dedupes + sorts + summarises,
runner loops until-converged.

The actual AI persona prompts are *not* tested here — they're prompt
templates, not algorithms. The contract is that ``prompt_fn(ctx)``
returns a non-empty str containing the project_root.
"""

from __future__ import annotations

import json
from dataclasses import asdict
from pathlib import Path

import pytest

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
from esp_harness.adversarial.runner import (
    dry_run_dispatcher,
    manual_dispatcher_factory,
    run_session,
)


# ──────────────────────────────────────────────────────────────────
# Registry self-registration
# ──────────────────────────────────────────────────────────────────


def test_builtin_personas_registered() -> None:
    names = {p.name for p in all_personas()}
    assert "verify" in names
    assert "falsify" in names


def test_persona_prompt_fn_returns_str() -> None:
    p = get_persona("verify")
    assert p is not None
    ctx = PersonaContext(project_root="/some/root")
    prompt = p.prompt_fn(ctx)
    assert isinstance(prompt, str)
    assert "/some/root" in prompt
    assert len(prompt) > 100


def test_falsify_uses_prior_findings_in_prompt() -> None:
    p = get_persona("falsify")
    assert p is not None
    prior = [
        Finding(
            id="F-verify-001", persona="verify", severity="blocking",
            what_broke="thing broke", evidence={}, code_location="x.py:10",
            suggested_fix="fix it",
        )
    ]
    ctx = PersonaContext(project_root="/r", prior_findings=prior)
    prompt = p.prompt_fn(ctx)
    assert "thing broke" in prompt
    assert "x.py:10" in prompt


# ──────────────────────────────────────────────────────────────────
# Aggregator
# ──────────────────────────────────────────────────────────────────


def _f(id_, persona, sev, where, conf=0.5):
    return Finding(
        id=id_, persona=persona, severity=sev, what_broke=f"{id_} broke",
        evidence={}, code_location=where, suggested_fix="fix",
        confidence=conf,
    )


def test_dedupe_merges_same_location_and_severity() -> None:
    findings = [
        _f("F-1", "verify",  "blocking", "a.py:10"),
        _f("F-2", "falsify", "blocking", "a.py:10"),
        _f("F-3", "verify",  "minor",    "a.py:10"),
        _f("F-4", "falsify", "blocking", "b.py:20"),
    ]
    out = dedupe(findings)
    assert len(out) == 3   # F-1 + F-2 merge; F-3 distinct severity; F-4 distinct loc
    # F-1's confidence should be bumped because F-2 corroborated.
    a_blocking = next(f for f in out if f.code_location == "a.py:10" and f.severity == "blocking")
    assert a_blocking.confidence == pytest.approx(0.6, abs=0.01)
    assert "corroborated_by" in a_blocking.evidence


def test_severity_sort_critical_first() -> None:
    findings = [
        _f("F-1", "verify",  "minor",    "a.py:10"),
        _f("F-2", "falsify", "critical", "b.py:10"),
        _f("F-3", "verify",  "blocking", "c.py:10"),
        _f("F-4", "falsify", "informational", "d.py:10"),
    ]
    out = severity_sort(findings)
    assert [f.id for f in out] == ["F-2", "F-3", "F-1", "F-4"]


def test_summarise_reports_converged_when_no_critical() -> None:
    findings = [
        _f("F-1", "verify", "blocking", "a.py:10"),
        _f("F-2", "verify", "minor",    "b.py:10"),
    ]
    s = summarise(findings)
    assert s["total"] == 2
    assert s["by_severity"]["blocking"] == 1
    assert s["by_severity"]["minor"] == 1
    assert s["by_severity"]["critical"] == 0
    assert s["converged"] is True


def test_summarise_reports_not_converged_with_critical() -> None:
    findings = [_f("F-1", "falsify", "critical", "a.py:10")]
    s = summarise(findings)
    assert s["converged"] is False
    assert len(s["critical_first"]) == 1


# ──────────────────────────────────────────────────────────────────
# Dispatcher + runner
# ──────────────────────────────────────────────────────────────────


def test_dry_run_dispatcher_emits_nothing(capsys) -> None:
    ctx = PersonaContext(project_root="/r")
    out = dry_run_dispatcher("verify", ctx)
    captured = capsys.readouterr()
    assert out == []
    assert "[dry-run]" in captured.out
    assert "/r" in captured.out


def test_manual_dispatcher_reads_jsonl(tmp_path: Path) -> None:
    fx = tmp_path / "f.jsonl"
    fx.write_text(json.dumps({
        "id": "F-falsify-001",
        "persona": "falsify",
        "severity": "critical",
        "what_broke": "demo",
        "evidence": {},
        "code_location": "demo.py:1",
        "suggested_fix": "fix it",
    }) + "\n", encoding="utf-8")
    disp = manual_dispatcher_factory(fx)
    ctx = PersonaContext(project_root="/r")
    out = disp("falsify", ctx)
    assert len(out) == 1
    assert out[0].severity == "critical"


def test_run_session_converges_when_dispatcher_emits_no_critical(tmp_path: Path) -> None:
    fx = tmp_path / "no-critical.jsonl"
    fx.write_text(json.dumps({
        "id": "F-verify-001", "persona": "verify", "severity": "blocking",
        "what_broke": "x", "evidence": {}, "code_location": "x.py:1",
        "suggested_fix": "y",
    }) + "\n", encoding="utf-8")
    out_dir = tmp_path / "out"
    summary = run_session(
        project_root="/r",
        persona_names=["verify"],
        rounds=2,
        until_converged=True,
        dispatcher=manual_dispatcher_factory(fx),
        findings_out=out_dir,
    )
    assert summary["converged"] is True
    assert summary["by_severity"]["blocking"] == 1
    assert (out_dir / "round-01.json").exists()
    assert (out_dir / "summary.json").exists()


def test_run_session_does_not_converge_on_critical(tmp_path: Path) -> None:
    fx = tmp_path / "critical.jsonl"
    fx.write_text(json.dumps({
        "id": "F-falsify-001", "persona": "falsify", "severity": "critical",
        "what_broke": "bad", "evidence": {}, "code_location": "x.py:1",
        "suggested_fix": "y",
    }) + "\n", encoding="utf-8")
    summary = run_session(
        project_root="/r",
        persona_names=["falsify"],
        rounds=2,
        until_converged=True,
        dispatcher=manual_dispatcher_factory(fx),
    )
    assert summary["converged"] is False
    assert summary["by_severity"]["critical"] == 1
