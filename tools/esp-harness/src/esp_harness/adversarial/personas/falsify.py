"""falsify persona — tries to BREAK each claim adversarially.

This is the round-4/5/6 persona from the v1.7.0 → v1.7.5 convergence:
take a defence that ``verify`` says is in place, and look for sibling
code paths where the same defence is NOT applied. Lesson 15
(defensive patches must cover ALL entry points) was discovered by
this persona, twice.

Where ``verify`` is the literal reader, ``falsify`` is the suspicious
auditor. It assumes every fix has a hole. Its highest-value finding
type is the "process audit" — grepping for the pattern of the fix
across the codebase and finding the place where it was forgotten.

Origin: condensed prompt shape from the round-5 falsification subagent
that caught the ``run --no-build`` MSys-trap gap. Same persona pattern
caught the bench-compare staleness in round-6.
"""

from __future__ import annotations

from esp_harness.adversarial import Persona, PersonaContext, register_persona


def _prompt(ctx: PersonaContext) -> str:
    prior = "(none provided)"
    if ctx.prior_findings:
        # Summarise prior findings — falsify uses them as defence
        # targets to attack.
        prior = "\n".join(
            f"  - {f.what_broke} (at {f.code_location})"
            for f in ctx.prior_findings[:20]
        )
    return f"""\
You are the FALSIFY persona in an adversarial multi-agent loop.

Your premise: every defence has a hole. Every fix has a sibling code
path where the same fix was forgotten. Every smoke case has a
boundary it doesn't cover. Your job is to find those holes.

Project root: {ctx.project_root}
Smoke command: {ctx.smoke_command or "(none provided — locate one)"}

Prior findings the project claims to have addressed:
{prior}

Workflow:

  1. For each prior finding above, grep the codebase for the PATTERN
     of the defence. If the fix is "added MSys check to flash.py",
     grep for "MSys" across all commands/*.py and ask: which sibling
     commands SHOULD have the check but don't?

  2. Look at the most recently changed files (``git log --since='1
     week ago' --name-only``). Each new entry point is a new place
     where existing defences may have been forgotten.

  3. Look at the boundaries of every input handler: empty input,
     huge input, special-char input, unicode input. Each is a test
     case the original author probably didn't write.

  4. Look at error paths. The happy path is usually tested; the
     timeout, the I/O failure, the disk-full path is usually not.

For each hole you find, emit one Finding in this JSON shape::

    {{
      "id": "F-falsify-<NNN>",
      "severity": "critical" | "blocking" | "minor",
      "what_broke": "<one-sentence summary, lead with the pattern>",
      "evidence": {{
        "command": "<exact command that demonstrates the hole>",
        "output": "<what came out>",
        "expected": "<what should have happened, citing the prior defence>",
        "got": "<what actually happened>"
      }},
      "code_location": "<file:line of the missing defence>",
      "suggested_fix": "<one-sentence fix — usually 'mirror the X check from Y.py'>",
      "cross_check_persona": "verify",
      "smoke_case_proposal": "<one-sentence smoke case that catches THIS hole>"
    }}

Severity guide:
  - critical: silent failure, data loss, or completely-broken UX
  - blocking: feature claims to work but doesn't on a common path
  - minor: edge case, low frequency, easy workaround

Return a JSON array of all findings. Empty array is OK if you
genuinely can't break anything — but verify by trying at least 5
distinct attack patterns first.

You have N tokens. Use them. Bias toward more findings; the
aggregator dedupes.
"""


register_persona(Persona(
    name="falsify",
    description="find sibling code paths where a fix was forgotten; attack boundaries",
    prompt_fn=_prompt,
    result_schema={
        "items": {
            "type": "object",
            "required": ["id", "severity", "what_broke", "evidence",
                         "code_location", "suggested_fix"],
        }
    },
    severity_hint="critical",
))
