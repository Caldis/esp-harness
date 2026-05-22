"""`esp-harness bench` — standardised performance snapshot.

Captures a fixed set of measurements so we can compare two firmware
versions side-by-side: idle FPS, internal/PSRAM heap, audio loopback
elapsed-vs-requested, BLE adv-event rate, SD bench KB/s, IMU noise
floor. Output is one JSON document with all fields populated (or null
on failure) so diffs are mechanical.

Why this command exists: ad-hoc benchmarking by re-typing console
commands and copy-pasting numbers is error-prone and not reproducible.
This composes those probes once, in a known order, with consistent
timing.

Quick mode (--quick) skips the longer probes (4 MB SD bench, 2 s BLE
scan) for a sub-3-second sanity check.
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

from esp_harness.core import ports as ports_mod
from esp_harness.core.console_session import ConsoleSession
from esp_harness.exit_codes import (
    AMBIGUOUS_DEVICE,
    DEVICE_BUSY,
    GENERIC_ERROR,
    NO_DEVICE,
    OK,
)
from esp_harness.output import Output


BASELINE_PATH = Path(__file__).resolve().parent.parent / "data" / "baseline.json"


# Regression check: (path, direction, threshold_pct).
# direction "down" = regression when current drops below baseline by threshold%
# direction "up"   = regression when current rises above baseline by threshold%
# Higher thresholds where the measurement is intrinsically noisy.
REGRESSION_CHECKS: list[tuple[tuple[str, ...], str, float]] = [
    (("stat", "fps"),                 "down", 10.0),
    # 10% (was 5%) — Aurora's audio loopback transiently allocates
    # ~440 KB PSRAM; if bench --baseline runs in a "calm" moment and
    # --compare runs right after a loopback, we see ~7% PSRAM drift
    # that isn't a real regression. 10% absorbs the natural transient
    # while still catching real leaks (which manifest as >>10% over
    # time).
    (("stat", "heap_free"),           "down", 10.0),
    (("stat", "int_free"),            "down", 10.0),
    (("stat", "psram_free"),          "down", 10.0),
    (("audio_loopback", "elapsed_ms"), "up",   20.0),
    (("ble_scan", "adv_events"),      "down", 40.0),  # BLE is noisy
    (("sd_bench", "write_kbps"),      "down", 20.0),
    (("sd_bench", "read_kbps"),       "down", 20.0),
]


def _dig(d: dict | None, path: tuple[str, ...]):
    """Walk nested dict by key tuple. Return None if any step missing."""
    cur: object = d
    for k in path:
        if not isinstance(cur, dict):
            return None
        cur = cur.get(k)
    return cur


def _compare(baseline: dict, current: dict) -> tuple[list[dict], list[dict]]:
    """Return (regressions, all_diffs). Both are lists of small dicts."""
    regressions: list[dict] = []
    diffs: list[dict] = []
    for path, direction, threshold in REGRESSION_CHECKS:
        base = _dig(baseline, path)
        curr = _dig(current, path)
        if base is None or curr is None:
            continue
        if not isinstance(base, (int, float)) or not isinstance(curr, (int, float)):
            continue
        if base == 0:
            continue
        delta_pct = (curr - base) / base * 100.0
        entry = {
            "metric": ".".join(path),
            "baseline": base,
            "current": curr,
            "delta_pct": round(delta_pct, 2),
            "direction": direction,
            "threshold_pct": threshold,
        }
        diffs.append(entry)
        regressed = (
            (direction == "down" and delta_pct < -threshold)
            or (direction == "up"   and delta_pct >  threshold)
        )
        if regressed:
            regressions.append(entry)
    return regressions, diffs


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser(
        "bench",
        help="Run a fixed set of perf probes and emit one JSON snapshot.",
        description=(
            "Captures fps + heap (?stat), audio playback timing, BLE adv "
            "event rate, SD read/write KB/s, IMU noise. Designed for "
            "cross-version regression tracking — diff the JSON to spot "
            "fps drops, heap leaks, etc."
        ),
    )
    p.add_argument("--port", default=None, help="COM port (auto-detect if omitted).")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument(
        "--quick", action="store_true",
        help="Skip the longer probes (SD bench, BLE scan).",
    )
    p.add_argument(
        "--out", type=Path, default=None,
        help="Write snapshot JSON to this path in addition to stdout.",
    )
    p.add_argument(
        "--baseline", action="store_true",
        help=(
            "Save this run as the package baseline "
            f"(default: {BASELINE_PATH.relative_to(BASELINE_PATH.parents[2])}). "
            "Use after a verified-good firmware to lock numbers."
        ),
    )
    p.add_argument(
        "--compare", action="store_true",
        help=(
            "Diff this run against the package baseline. Returns exit code "
            "1 if any metric regresses beyond its threshold; 0 otherwise. "
            "Combine with --quick for CI smoke checks."
        ),
    )
    add_common_flags(p)


def _resolve_port(requested: str | None, output: Output) -> tuple[str | None, int]:
    if requested:
        return requested, OK
    chosen, candidates = ports_mod.detect_one_esp_port()
    if chosen is not None:
        return chosen.port, OK
    if not candidates:
        output.failure(exit_code=NO_DEVICE, error="No ESP32 port found.")
        return None, NO_DEVICE
    output.failure(
        exit_code=AMBIGUOUS_DEVICE,
        error=f"Ambiguous: {len(candidates)} candidates. Pass --port.",
    )
    return None, AMBIGUOUS_DEVICE


def _send_json(session: ConsoleSession, cmd: str, timeout: float = 6.0) -> dict | None:
    """Send a command, parse OK: body as JSON. Return None on failure."""
    r = session.send(cmd, timeout=timeout)
    if not r.ok:
        return None
    try:
        return json.loads(r.text)
    except Exception:
        return None


def _probe_stat(session: ConsoleSession) -> dict:
    j = _send_json(session, "?stat") or {}
    return {
        "fps": j.get("fps"),
        "heap_free": j.get("heap_free"),
        "heap_min": j.get("heap_min"),
        "psram_free": j.get("psram_free"),
        "int_free": j.get("int_free"),
        "int_largest": j.get("int_largest"),
        "uptime_ms": j.get("uptime_ms"),
        "scene_count": j.get("scene_count"),
    }


def _probe_power(session: ConsoleSession) -> dict:
    return _send_json(session, "?power") or {}


def _probe_audio_loopback(session: ConsoleSession) -> dict:
    """Loopback 500 ms — captures both directions of the audio chain
    + the speaker boost path. Volume already capped by firmware vol
    setting (we don't change it here)."""
    j = _send_json(session, "audio loopback 500", timeout=4.0) or {}
    return {
        "requested_ms": j.get("requested_ms"),
        "elapsed_ms": j.get("elapsed_ms"),
        "peak_dbfs": j.get("peak_dbfs"),
        "rms_dbfs": j.get("rms_dbfs"),
    }


def _probe_ble(session: ConsoleSession, dur_ms: int) -> dict:
    j = _send_json(session, f"ble scan {dur_ms} 8", timeout=dur_ms / 1000.0 + 4.0) or {}
    return {
        "elapsed_ms": j.get("elapsed_ms"),
        "device_count": j.get("count"),
        "adv_events": j.get("adv_events"),
    }


def _probe_sd(session: ConsoleSession, mb: int) -> dict:
    j = _send_json(session, f"?sd bench {mb}", timeout=mb * 3.0 + 4.0) or {}
    return {
        "mb": j.get("mb"),
        "write_kbps": j.get("write_kbps"),
        "read_kbps": j.get("read_kbps"),
    }


def _probe_sensor_noise(session: ConsoleSession, samples: int = 5) -> dict:
    """Read accel a few times, return |a - mean|.max as a rough noise
    proxy. A working IMU shows ~0.001 g jitter; a stuck readback shows 0."""
    readings: list[list[float]] = []
    for _ in range(samples):
        j = _send_json(session, "?sensor", timeout=2.0) or {}
        a = j.get("accel")
        if isinstance(a, list) and len(a) == 3:
            readings.append([float(x) for x in a])
        time.sleep(0.05)
    if not readings:
        return {"samples": 0}
    n = len(readings)
    mean = [sum(r[i] for r in readings) / n for i in range(3)]
    max_dev = max(
        max(abs(r[i] - mean[i]) for i in range(3)) for r in readings
    )
    return {"samples": n, "mean_g": mean, "max_dev_g": max_dev}


def run(args: argparse.Namespace, output: Output) -> int:
    port, code = _resolve_port(args.port, output)
    if port is None:
        return code

    started = time.monotonic()
    snapshot: dict[str, object] = {
        "port": port,
        "quick": args.quick,
        "started_at_unix": time.time(),
    }

    try:
        with ConsoleSession(port, baud=args.baud) as session:
            # Identify the firmware so a saved baseline is interpretable
            # later. Best-effort — older firmwares without ?sys still bench.
            sys_info = _send_json(session, "?sys", timeout=3.0) or {}
            if sys_info:
                snapshot["device"] = {
                    "app_name": sys_info.get("app"),
                    "app_version": sys_info.get("version"),
                    "idf": sys_info.get("idf"),
                    "elf_sha": sys_info.get("elf_sha"),
                    "reset_reason": sys_info.get("reset_reason"),
                }
            snapshot["stat"] = _probe_stat(session)
            snapshot["power"] = _probe_power(session)
            snapshot["audio_loopback"] = _probe_audio_loopback(session)
            snapshot["sensor_noise"] = _probe_sensor_noise(session)
            if not args.quick:
                snapshot["ble_scan"] = _probe_ble(session, 1500)
                snapshot["sd_bench"] = _probe_sd(session, 1)
            else:
                snapshot["ble_scan"] = {"skipped": True}
                snapshot["sd_bench"] = {"skipped": True}
    except Exception as e:
        msg = str(e)
        if any(k in msg.lower() for k in ("access", "permission", "busy")):
            output.failure(exit_code=DEVICE_BUSY,
                           error=f"Port {port} busy: {e}")
            return DEVICE_BUSY
        output.failure(exit_code=GENERIC_ERROR, error=f"bench: {e}")
        return GENERIC_ERROR

    elapsed_ms = int((time.monotonic() - started) * 1000)
    snapshot["elapsed_ms"] = elapsed_ms

    if args.out is not None:
        try:
            args.out.write_text(json.dumps(snapshot, indent=2), encoding="utf-8")
        except Exception as e:
            output.info(f"warn: failed to write --out {args.out}: {e}")

    # --baseline: persist this run as the package baseline.
    if args.baseline:
        try:
            BASELINE_PATH.parent.mkdir(parents=True, exist_ok=True)
            BASELINE_PATH.write_text(json.dumps(snapshot, indent=2), encoding="utf-8")
            output.info(f"baseline saved to {BASELINE_PATH}")
        except Exception as e:
            output.failure(exit_code=GENERIC_ERROR,
                           error=f"failed to write baseline: {e}")
            return GENERIC_ERROR

    # --compare: diff against the package baseline.
    if args.compare:
        if not BASELINE_PATH.exists():
            output.failure(exit_code=GENERIC_ERROR,
                           error=f"no baseline at {BASELINE_PATH} (run --baseline first)")
            return GENERIC_ERROR
        try:
            baseline = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
        except Exception as e:
            output.failure(exit_code=GENERIC_ERROR,
                           error=f"failed to read baseline: {e}")
            return GENERIC_ERROR
        regressions, diffs = _compare(baseline, snapshot)
        snapshot["compare"] = {
            "baseline_started_at": baseline.get("started_at_unix"),
            "baseline_device": baseline.get("device"),
            "current_device": snapshot.get("device"),
            "regressions": regressions,
            "diffs": diffs,
        }
        if regressions:
            output.failure(
                exit_code=GENERIC_ERROR,
                error=f"{len(regressions)} regression(s) vs baseline",
                details={
                    "regressions": regressions,
                    "all_diffs": diffs,
                    "snapshot": snapshot,
                },
            )
            return GENERIC_ERROR

    output.success(snapshot, human=(
        f"bench snapshot · {elapsed_ms} ms · "
        f"fps={snapshot['stat'].get('fps')} "  # type: ignore[union-attr]
        f"heap={snapshot['stat'].get('heap_free')}"
    ))
    return OK
