"""Tests for module registry."""
from __future__ import annotations

from pathlib import Path

import pytest

from esp_harness.core.modules import MODULE_REGISTRY, get_module, list_all_modules


def test_registry_has_core_modules():
    names = [m.name for m in MODULE_REGISTRY]
    assert "scenes" in names
    assert "console" in names
    assert "toast" in names
    assert "bridge" in names
    assert "sim" in names


def test_get_module_returns_module():
    m = get_module("scenes")
    assert m is not None
    assert m.name == "scenes"
    assert m.default is True


def test_get_module_returns_none_for_unknown():
    assert get_module("nonexistent") is None


def test_list_all_modules():
    modules = list_all_modules()
    assert len(modules) >= 5
    for m in modules:
        assert hasattr(m, "name")
        assert hasattr(m, "default")
        assert hasattr(m, "description")


def test_scaffold_creates_files(tmp_path: Path):
    m = get_module("bridge")
    assert m is not None
    created = m.scaffold(tmp_path, project_name="test_proj")
    assert len(created) > 0
    for f in created:
        assert (tmp_path / f).exists(), f"Expected {f} to exist"


def test_remove_deletes_files(tmp_path: Path):
    m = get_module("bridge")
    assert m is not None
    created = m.scaffold(tmp_path, project_name="test_proj")
    removed = m.remove(tmp_path)
    for f in removed:
        assert not (tmp_path / f).exists(), f"Expected {f} to be removed"


def test_manifest_fragment():
    m = get_module("bridge")
    assert m is not None
    frag = m.manifest_fragment()
    assert isinstance(frag, dict)
    assert "name" in frag
