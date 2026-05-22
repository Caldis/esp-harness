"""esp-harness adversarial — multi-persona falsification harness.

The "v1.8 north star" from the agent-dashboard project (see
``docs/ADVERSARIAL_AS_PRIMITIVE.md`` over there for the long-form
motivation). Captures the six-round manual adversarial loop that
took the framework from v1.7.0 → v1.7.5 and bakes it in as a
first-class command::

    esp-harness adversarial \\
        --personas verify,falsify \\
        --rounds 3 \\
        --findings-out ./findings/ \\
        --project .

Each persona is a prompt template + a result-shape contract. The
command spawns one subagent per persona per round, collects
findings in a uniform schema, dedupes, cross-checks via a second
persona, and (with ``--auto-fix``) spawns fixer agents to propose
patches.

This v0 scaffold ships:

  - the persona registry (``register_persona``)
  - two personas (verify, falsify) ported from the round-4/5/6
    subagent prompts that proved the pattern works
  - the aggregator (dedupe by code-location, cross-check by
    second persona)
  - the CLI (``esp-harness adversarial``)
  - a *dry-run dispatcher* that prints the prompts that WOULD be
    sent to subagents — enough to exercise the plumbing without
    requiring an active AI session

The real AI dispatcher is wired separately: when the command runs
inside Claude Code or with the Anthropic SDK on the path, it
spawns parallel subagents and collects findings. The v0 scaffold
defers that to v0.3.1; the abstraction (``PersonaDispatcher``) is
in place.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Optional

# ──────────────────────────────────────────────────────────────────
# Persona registry
# ──────────────────────────────────────────────────────────────────


@dataclass
class Persona:
    """One adversarial perspective.

    ``prompt_fn`` takes the orchestrator-supplied project context and
    returns the prompt string the subagent receives. ``result_schema``
    documents what the persona is expected to return (the runner uses
    it to validate findings).
    """
    name: str
    description: str
    prompt_fn: Callable[["PersonaContext"], str]
    result_schema: dict
    severity_hint: str = "minor"


@dataclass
class PersonaContext:
    """Inputs the runner passes to each persona's prompt_fn."""
    project_root: str
    smoke_command: Optional[str] = None      # e.g. "pwsh tools/smoke.ps1"
    known_lessons_path: Optional[str] = None # e.g. "docs/lessons-v1.7.md"
    prior_findings: list = field(default_factory=list)
    extra_context: dict = field(default_factory=dict)


_REGISTRY: dict[str, Persona] = {}


def register_persona(persona: Persona) -> None:
    """Add a persona to the global registry. Idempotent on name."""
    _REGISTRY[persona.name] = persona


def get_persona(name: str) -> Optional[Persona]:
    return _REGISTRY.get(name)


def all_personas() -> list[Persona]:
    return list(_REGISTRY.values())


# Eager-import the built-in personas so they self-register.
from esp_harness.adversarial.personas import verify  # noqa: E402,F401
from esp_harness.adversarial.personas import falsify  # noqa: E402,F401


# ──────────────────────────────────────────────────────────────────
# Findings schema
# ──────────────────────────────────────────────────────────────────


@dataclass
class Finding:
    """One adversarial discovery."""
    id: str
    persona: str
    severity: str           # critical | blocking | minor | informational
    what_broke: str
    evidence: dict          # {command, output, expected, got}
    code_location: str
    suggested_fix: str
    cross_check_persona: Optional[str] = None
    smoke_case_proposal: Optional[str] = None
    confidence: float = 0.5
    round_n: int = 0

    def dedupe_key(self) -> str:
        """Findings with the same code-location + severity are duplicates."""
        return f"{self.code_location}::{self.severity}"


__all__ = [
    "Persona",
    "PersonaContext",
    "Finding",
    "register_persona",
    "get_persona",
    "all_personas",
]
