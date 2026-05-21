"""Dual-mode output: human text vs JSON.

Every command uses this to emit results. The same payload is rendered two ways
depending on `--json`. JSON output is the contract for agents; human output is
for humans (and is allowed to evolve freely).
"""

from __future__ import annotations

import json
import sys
from typing import Any


class Output:
    """Output sink. Construct once per command invocation."""

    def __init__(self, *, json_mode: bool = False, verbose: bool = False):
        self.json_mode = json_mode
        self.verbose = verbose

    # ── human-facing logs (silent in JSON mode) ───────────────────────
    def info(self, msg: str) -> None:
        if not self.json_mode:
            print(msg, file=sys.stderr)

    def warn(self, msg: str) -> None:
        if not self.json_mode:
            print(f"[warn] {msg}", file=sys.stderr)

    def debug(self, msg: str) -> None:
        if self.verbose and not self.json_mode:
            print(f"[debug] {msg}", file=sys.stderr)

    # ── result emission (stdout) ──────────────────────────────────────
    def success(self, payload: dict[str, Any], *, human: str | None = None) -> None:
        """Emit a success result. Always exits 0 unless caller decides otherwise."""
        if self.json_mode:
            print(json.dumps({"ok": True, **payload}, ensure_ascii=False))
        else:
            if human is not None:
                print(human)
            else:
                # default human render: key: value lines
                for k, v in payload.items():
                    print(f"{k}: {v}")

    def failure(
        self,
        *,
        exit_code: int,
        error: str,
        details: dict[str, Any] | None = None,
        human: str | None = None,
    ) -> None:
        """Emit a failure result. Caller still has to sys.exit(exit_code)."""
        if self.json_mode:
            payload: dict[str, Any] = {"ok": False, "error": error, "exit_code": exit_code}
            if details:
                payload.update(details)
            print(json.dumps(payload, ensure_ascii=False))
        else:
            print(f"[error] {error}", file=sys.stderr)
            if human:
                print(human, file=sys.stderr)
            if details and self.verbose:
                for k, v in details.items():
                    print(f"  {k}: {v}", file=sys.stderr)
