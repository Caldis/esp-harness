"""Findings aggregator — dedupe + cross-check + severity-classify.

Takes the raw findings emitted by N personas across M rounds and
produces a consolidated list that survives basic sanity checks::

  - duplicate findings (same code_location + severity) are merged;
    the highest-confidence wins, the rest become evidence of
    independent corroboration (which bumps confidence).
  - findings flagged with ``cross_check_persona`` are re-run through
    that second persona before being kept. Findings that the second
    persona can't reproduce drop to "informational" severity.
  - severity is sorted descending: critical → blocking → minor →
    informational.

The aggregator is intentionally pure-Python — it operates on
:class:`Finding` lists and returns a list, no I/O. The runner
handles dispatch + persistence.
"""

from __future__ import annotations

from dataclasses import asdict
from typing import Callable, Optional

from esp_harness.adversarial import Finding


_SEVERITY_ORDER = {
    "critical": 0,
    "blocking": 1,
    "minor": 2,
    "informational": 3,
}


def dedupe(findings: list[Finding]) -> list[Finding]:
    """Merge findings that share (code_location, severity).

    Merged entries get a confidence boost (capped at 1.0) and their
    secondary evidence gets folded into ``evidence['corroborated_by']``.
    """
    bucket: dict[str, Finding] = {}
    for f in findings:
        key = f.dedupe_key()
        if key not in bucket:
            bucket[key] = f
            continue
        primary = bucket[key]
        primary.confidence = min(1.0, primary.confidence + 0.1)
        corroborated = primary.evidence.setdefault("corroborated_by", [])
        corroborated.append({
            "persona": f.persona,
            "id": f.id,
            "what_broke": f.what_broke,
        })
    return list(bucket.values())


def severity_sort(findings: list[Finding]) -> list[Finding]:
    """Sort findings descending by severity (critical first)."""
    return sorted(
        findings,
        key=lambda f: (_SEVERITY_ORDER.get(f.severity, 99), -f.confidence)
    )


def cross_check(
    findings: list[Finding],
    dispatcher: Callable[[str, dict], list[Finding]],
) -> list[Finding]:
    """For findings with a ``cross_check_persona`` hint, re-run that
    persona on the original finding and demote ones that don't survive.

    ``dispatcher`` is the runner's persona-spawn callback; it takes
    ``(persona_name, prior_findings_payload)`` and returns the cross-
    checked findings. If a cross-check returns no Finding for the
    original code-location, the original drops to "informational".
    """
    out = []
    for f in findings:
        if not f.cross_check_persona:
            out.append(f)
            continue
        sub_findings = dispatcher(
            f.cross_check_persona,
            {"target_finding": asdict(f)},
        )
        if not any(s.code_location == f.code_location for s in sub_findings):
            # Cross-checker couldn't reproduce — demote.
            f.severity = "informational"
            f.confidence *= 0.5
        out.append(f)
    return out


def summarise(findings: list[Finding]) -> dict:
    """One JSON-serialisable summary suitable for `--findings-out`."""
    by_severity = {sev: 0 for sev in _SEVERITY_ORDER}
    for f in findings:
        by_severity[f.severity] = by_severity.get(f.severity, 0) + 1
    return {
        "total": len(findings),
        "by_severity": by_severity,
        "critical_first": [
            {"id": f.id, "what_broke": f.what_broke, "where": f.code_location}
            for f in findings if f.severity == "critical"
        ],
        "converged": by_severity.get("critical", 0) == 0,
    }
