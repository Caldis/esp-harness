"""harness.json config loader — the project's single source of truth.

Every CLI command reads defaults from this file. If harness.json is
absent, commands fall back to CLI args (backward compat).
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

CONFIG_FILENAME = "harness.json"
MAX_WALK_UP = 10


@dataclass
class HarnessConfig:
    name: str
    version: str
    board: str
    port: str
    modules: dict[str, bool]
    agent_bootstrap: str
    agent_verify: str
    agent_cycle: list[str]
    config_path: Path  # directory containing harness.json

    def enabled_modules(self) -> list[str]:
        return sorted(k for k, v in self.modules.items() if v)

    def save(self) -> None:
        data = {
            "name": self.name,
            "version": self.version,
            "board": self.board,
            "port": self.port,
            "modules": self.modules,
            "agent": {
                "bootstrap": self.agent_bootstrap,
                "verify": self.agent_verify,
                "cycle": self.agent_cycle,
            },
        }
        path = self.config_path / CONFIG_FILENAME
        path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def load_config(start_dir: Path | None = None) -> HarnessConfig | None:
    """Walk up from start_dir looking for harness.json. Return None if not found."""
    d = (start_dir or Path.cwd()).resolve()
    for _ in range(MAX_WALK_UP):
        candidate = d / CONFIG_FILENAME
        if candidate.is_file():
            return _parse(candidate, d)
        parent = d.parent
        if parent == d:
            break
        d = parent
    return None


def _parse(path: Path, config_dir: Path) -> HarnessConfig | None:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return None
    agent = raw.get("agent", {})
    return HarnessConfig(
        name=raw.get("name", ""),
        version=raw.get("version", "0.1.0"),
        board=raw.get("board", ""),
        port=raw.get("port", ""),
        modules=raw.get("modules", {}),
        agent_bootstrap=agent.get("bootstrap", "esp-harness manifest --json"),
        agent_verify=agent.get("verify", "esp-harness verify"),
        agent_cycle=agent.get("cycle", ["build", "flash", "verify"]),
        config_path=config_dir,
    )
