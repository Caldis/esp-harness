"""Manifest sanity: toolkit_commands inventory matches the actual
registered subcommands. (This is what `tools/check_manifest.py` checks
too, but as a proper pytest case it shows up in test reports.)"""

EXPECTED_MIN_TOOLKIT_CMDS = 14  # bumps as more commands land

def test_manifest_returns_toolkit_inventory(esp_harness):
    rc, payload, stderr = esp_harness("manifest", timeout=20)
    assert rc == 0, f"manifest exit {rc}; stderr:\n{stderr}"
    assert payload is not None
    toolkit_cmds = payload.get("toolkit_commands") or []
    assert len(toolkit_cmds) >= EXPECTED_MIN_TOOLKIT_CMDS, \
        f"only {len(toolkit_cmds)} toolkit commands listed; expected >= {EXPECTED_MIN_TOOLKIT_CMDS}"
    # Every command must at minimum have a name + summary
    for c in toolkit_cmds:
        assert "name" in c and "summary" in c, f"malformed manifest entry: {c}"
