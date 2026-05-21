"""Sim diff regression: the host build of the showcase reproduces the
golden snapshots. Real visual regressions land here as a red bar."""
import pytest

DEFAULT_SCENES = "halo,grid,bloom,tilt,pulse,cell,keys,tone,system,glow,spin,notify,track"


def test_sim_diff_all_scenes_pass(esp_harness, sim_binary):
    rc, payload, stderr = esp_harness(
        "sim",
        "--bin", str(sim_binary),
        "diff",
        "--scenes", DEFAULT_SCENES,
        timeout=120,
    )
    assert rc == 0, (
        f"sim diff exit {rc}; "
        f"failed={payload.get('failed') if payload else 'unknown'}; "
        f"stderr:\n{stderr}"
    )
    assert payload is not None
    assert payload.get("failed") in (None, [], ()), \
        f"unexpected regressions: {payload.get('failed')}"
    # Every scene listed must have a per-scene status entry
    assert len(payload.get("scenes", [])) == len(DEFAULT_SCENES.split(","))
