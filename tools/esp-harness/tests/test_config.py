"""Tests for harness.json config loader."""
from __future__ import annotations

import json
from pathlib import Path

import pytest

from esp_harness.core.config import load_config, HarnessConfig, CONFIG_FILENAME


def test_load_from_directory(tmp_path: Path):
    cfg = {
        "name": "test-project",
        "version": "0.1.0",
        "board": "esp32_s3_touch_amoled_2_16",
        "port": "COM9",
        "modules": {"scenes": True, "console": True, "bridge": False},
        "agent": {
            "bootstrap": "esp-harness manifest --json",
            "verify": "esp-harness verify",
            "cycle": ["build", "flash", "verify"],
        },
    }
    (tmp_path / CONFIG_FILENAME).write_text(json.dumps(cfg), encoding="utf-8")
    result = load_config(tmp_path)
    assert result is not None
    assert result.name == "test-project"
    assert result.board == "esp32_s3_touch_amoled_2_16"
    assert result.port == "COM9"
    assert result.modules["scenes"] is True
    assert result.modules["bridge"] is False
    assert result.agent_cycle == ["build", "flash", "verify"]


def test_load_returns_none_when_missing(tmp_path: Path):
    result = load_config(tmp_path)
    assert result is None


def test_load_walks_up(tmp_path: Path):
    cfg = {"name": "parent-project", "version": "0.1.0", "board": "test", "port": "COM3", "modules": {}, "agent": {}}
    (tmp_path / CONFIG_FILENAME).write_text(json.dumps(cfg), encoding="utf-8")
    child = tmp_path / "main" / "scenes"
    child.mkdir(parents=True)
    result = load_config(child)
    assert result is not None
    assert result.name == "parent-project"


def test_enabled_modules():
    cfg = HarnessConfig(
        name="t", version="0.1.0", board="b", port="COM1",
        modules={"scenes": True, "console": True, "bridge": False},
        agent_bootstrap="", agent_verify="", agent_cycle=[],
        config_path=Path("."),
    )
    assert cfg.enabled_modules() == ["console", "scenes"]


def test_save_roundtrip(tmp_path: Path):
    cfg = HarnessConfig(
        name="rt", version="0.2.0", board="myboard", port="COM5",
        modules={"scenes": True, "sim": False},
        agent_bootstrap="esp-harness manifest --json",
        agent_verify="esp-harness verify",
        agent_cycle=["build", "flash", "verify"],
        config_path=tmp_path,
    )
    cfg.save()
    reloaded = load_config(tmp_path)
    assert reloaded is not None
    assert reloaded.name == "rt"
    assert reloaded.port == "COM5"
    assert reloaded.modules == {"scenes": True, "sim": False}
