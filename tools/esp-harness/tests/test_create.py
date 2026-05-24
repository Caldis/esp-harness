"""Tests for esp-harness create."""
from __future__ import annotations

import json
from pathlib import Path

import pytest

from esp_harness.commands.create import run_create
from esp_harness.core.config import load_config, CONFIG_FILENAME


def test_create_generates_project(tmp_path: Path):
    target = tmp_path / "my-project"
    result = run_create("my-project", target, board="test_board", port="COM3")
    assert result == 0
    assert (target / CONFIG_FILENAME).exists()
    assert (target / "CLAUDE.md").exists()
    assert (target / "main" / "my_project_main.c").exists()
    assert (target / "main" / "scenes" / "scene_hello.c").exists()
    assert (target / "CMakeLists.txt").exists()

    cfg = load_config(target)
    assert cfg is not None
    assert cfg.name == "my-project"
    assert cfg.board == "test_board"
    assert cfg.port == "COM3"
    assert cfg.modules["scenes"] is True
    assert cfg.modules["bridge"] is False


def test_create_claude_md_contains_bootstrap(tmp_path: Path):
    target = tmp_path / "proj"
    run_create("proj", target, board="b", port="COM1")
    claude_md = (target / "CLAUDE.md").read_text(encoding="utf-8")
    assert "esp-harness manifest --json" in claude_md


def test_create_fails_if_target_exists(tmp_path: Path):
    target = tmp_path / "existing"
    target.mkdir()
    (target / "something.txt").write_text("occupied")
    result = run_create("existing", target, board="b", port="COM1")
    assert result != 0
