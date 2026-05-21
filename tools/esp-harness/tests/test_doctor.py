"""Doctor: the most basic test — toolkit env is healthy enough to
function. Any required dep missing should turn this red on CI."""

def test_doctor_passes(esp_harness):
    rc, payload, stderr = esp_harness("doctor", timeout=20)
    assert rc == 0, f"doctor exit {rc}; stderr:\n{stderr}"
    assert payload is not None
    assert payload["ok"] is True
    assert payload["n_missing_required"] == 0
    # Required checks: idf, cmake, Pillow, pyserial
    required = [c for c in payload["checks"] if c.get("required")]
    assert all(c["status"] == "ok" for c in required), \
        f"required failing: {[c for c in required if c['status'] != 'ok']}"
