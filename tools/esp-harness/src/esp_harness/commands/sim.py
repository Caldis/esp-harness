"""`esp-harness sim` — drive a host LVGL simulator binary.

PROJECT-AGNOSTIC after the G-6 cleanup (May 2026). Pre-G-6 this
command had Aurora's 13 scene names hardcoded as defaults and auto-
detected `aurora_sim.exe` if `--bin` was omitted — meaning ANY
consumer running `esp-harness sim diff` got Aurora's visual gates
by default. That coupling is now removed: callers must pass --bin
explicitly, and the scene-name → index map is loaded from a
`scenes.json` next to the binary (each consumer ships their own).

Subcommands:
  snapshot     start a scene, run for N ms, dump SDL window to BMP, exit.
  diff         snapshot N scenes + compare to consumer's golden dir.
  update-golden refresh consumer's goldens.
  record       capture an animation timeline.

Aurora's own configuration lives at
`examples/aurora/sim/scenes.json`; Aurora's tests pass
`--bin examples/aurora/sim/build/aurora_sim.exe` explicitly. This
file is no longer hardcoded with Aurora's content.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from esp_harness.exit_codes import CLI_MISUSE, GENERIC_ERROR, OK, PROJECT_NOT_FOUND
from esp_harness.output import Output


# Conservative default if a consumer's scenes.json doesn't set one.
DEFAULT_THRESHOLD = 0.01


def _load_scene_config(binary: Path) -> dict:
    """Read `<binary parent>/scenes.json` for the consumer's scene
    name→index map + per-scene tolerances + default threshold.

    Shape:
        {"scenes": ["s0", "s1", ...],          # list = positional index map
         "tolerances": {"sN": 0.05, ...},      # optional per-scene override
         "default_threshold": 0.01}            # optional global default

    The simulator binary's own `scene_fw_register()` order MUST match
    the `scenes` list — that's the contract the consumer is asserting
    by shipping this file alongside the binary.

    Returns an empty default config if the file isn't found. The CLI
    then fails any sub-command that needs the map with a clear error
    (see _scene_index)."""
    # `sim/build/aurora_sim.exe` → look at sim/scenes.json (peer dir).
    candidate = binary.parent.parent / "scenes.json"
    if not candidate.exists():
        return {"scenes": [], "tolerances": {}, "default_threshold": DEFAULT_THRESHOLD}
    try:
        return json.loads(candidate.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return {"scenes": [], "tolerances": {}, "default_threshold": DEFAULT_THRESHOLD}


def _scene_index(name: str, scenes_list: list[str]) -> int | None:
    try:
        return scenes_list.index(name)
    except ValueError:
        return None


def _default_golden_dir(binary: Path) -> Path:
    """Golden dir is a sibling of the sim binary's `build/` parent —
    so `sim/build/<bin>` → `sim/golden/`. Each consumer keeps their
    own goldens; the framework no longer assumes a `<showcase>` layout."""
    return binary.parent.parent / "golden"


def _run_snapshot(binary: Path, scene_idx: int, out: Path, ms: int) -> tuple[bool, str]:
    cmd = [
        str(binary),
        "--scene", str(scene_idx),
        "--exit-after-ms", str(ms),
        "--snapshot", str(out.resolve()),
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=ms / 1000 + 10)
    except subprocess.TimeoutExpired:
        return False, "timed out"
    if proc.returncode != 0:
        return False, f"exit {proc.returncode}: {proc.stderr[-200:]}"
    if not out.exists():
        return False, "binary ran but wrote no file"
    return True, ""


def _bmp_diff_ratio(a_path: Path, b_path: Path, pixel_tol: int) -> tuple[float, str]:
    """Return (fraction_of_differing_pixels, descriptive_note).

    Uses Pillow if available — required for meaningful per-pixel diff
    across 800KB BMPs without numpy."""
    try:
        from PIL import Image, ImageChops
    except ImportError:
        return -1.0, "Pillow not installed — install with `pip install Pillow`"

    a = Image.open(a_path).convert("RGB")
    b = Image.open(b_path).convert("RGB")
    if a.size != b.size:
        return 1.0, f"size mismatch {a.size} vs {b.size}"

    diff = ImageChops.difference(a, b)
    if not diff.getbbox():
        return 0.0, "identical"

    # Reduce per-pixel RGB difference to single-channel max via channel split
    # + lighter() image-compositing — gives "the largest per-channel delta
    # at each pixel" which we then histogram for tolerance bucketing.
    r, g, bb = diff.split()
    max_chan = ImageChops.lighter(ImageChops.lighter(r, g), bb)
    hist = max_chan.histogram()
    total = sum(hist)
    differing = sum(hist[pixel_tol + 1:])
    return differing / total if total else 0.0, ""


def _resolve_sim_binary(args_bin: Path | None,
                         args_project: Path | None) -> Path | None:
    """Find the sim binary for THIS invocation. No auto-detect of
    Aurora — each consumer points explicitly via --bin (preferred)
    or --project (where `sim/build/*` is conventional). Returns None
    if nothing matches; the caller turns that into a user-visible
    error with a clear "pass --bin" hint."""
    if args_bin:
        return args_bin if args_bin.exists() else None
    if args_project:
        for sub in ("sim/build",):
            sim_dir = args_project / sub
            if not sim_dir.exists():
                continue
            for candidate in sim_dir.iterdir():
                # Pick the first executable file in sim/build/.
                if candidate.is_file() and os.access(candidate, os.X_OK):
                    return candidate
                if candidate.suffix.lower() == ".exe" and candidate.is_file():
                    return candidate
    return None


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser(
        "sim",
        help="Drive a host LVGL simulator binary (consumer's own sim).",
        description=(
            "Wraps the host build of a consumer project's LVGL simulator. "
            "Project-agnostic — point --bin at the simulator binary, or --project "
            "at the project root (the toolkit looks for sim/build/*). The scene "
            "name->index map is loaded from `scenes.json` next to the binary."
        ),
    )
    p.add_argument(
        "--bin", type=Path, default=None,
        help="Path to the consumer's sim binary (e.g. examples/aurora/sim/build/aurora_sim.exe).",
    )
    p.add_argument(
        "--project", type=Path, default=None,
        help="Consumer project root (toolkit searches sim/build/* for the binary).",
    )
    sp = p.add_subparsers(dest="sim_subcommand", metavar="<subcommand>")
    sp.required = True

    # ── snapshot ───────────────────────────────────────────────────
    ss = sp.add_parser(
        "snapshot",
        help="Start a scene, run briefly, write a BMP, exit.",
    )
    ss.add_argument("--scene", type=int, default=0,
                    help="Starting scene index (default 0).")
    ss.add_argument("--ms", type=int, default=500,
                    help="Settle time before snapshot, ms (default 500).")
    ss.add_argument("--out", type=Path, required=True,
                    help="Destination BMP path.")
    add_common_flags(ss)

    # ── diff ──────────────────────────────────────────────────────
    sd = sp.add_parser(
        "diff",
        help="Snapshot N scenes, compare to golden/, exit 1 if any regresses.",
    )
    sd.add_argument("--scenes", required=True,
                    help="Comma-separated scene names matching the project's "
                         "scenes.json `scenes` list (no scene_ prefix).")
    sd.add_argument("--golden", type=Path, default=None,
                    help="Golden directory (default: <project>/sim/golden alongside the bin).")
    sd.add_argument("--ms", type=int, default=500,
                    help="Settle time per scene snapshot.")
    sd.add_argument("--threshold", type=float, default=None,
                    help="Max fraction of differing pixels per scene. If omitted, "
                         "uses per-scene tolerances from the project's scenes.json; "
                         "default fallback is 0.01.")
    sd.add_argument("--pixel-tol", type=int, default=8,
                    help="Per-channel tolerance before a pixel counts as different (0-255, default 8).")
    sd.add_argument("--save-diffs", type=Path, default=None,
                    help="If set, save per-scene side-by-side diff images here.")
    add_common_flags(sd)

    # ── update-golden ─────────────────────────────────────────────
    su = sp.add_parser(
        "update-golden",
        help="Refresh golden snapshots from current sim output. Use after intentional UI changes.",
    )
    su.add_argument("--scenes", required=True,
                    help="Comma-separated scene names to refresh.")
    su.add_argument("--golden", type=Path, default=None,
                    help="Golden directory (default: <project>/sim/golden alongside the bin).")
    su.add_argument("--ms", type=int, default=500)
    add_common_flags(su)

    # ── record ────────────────────────────────────────────────────
    sr = sp.add_parser(
        "record",
        help="Capture a sequence of snapshots across an animation timeline.",
        description=(
            "Spawns the sim binary N times against the same scene, each with "
            "a progressively longer --exit-after-ms, capturing the animation "
            "at deterministic phase points. Output: <out>/frame_000.bmp, "
            "frame_001.bmp, ...  Use this on animated scenes to review motion."
        ),
    )
    sr.add_argument("--scene", required=True,
                    help="Scene name (see manifest) or numeric index.")
    sr.add_argument("--frames", type=int, default=10,
                    help="How many snapshots to capture (default 10).")
    sr.add_argument("--interval", type=int, default=100,
                    help="ms between capture points (default 100).")
    sr.add_argument("--settle", type=int, default=200,
                    help="ms of warmup before first snapshot (default 200).")
    sr.add_argument("--out", type=Path, required=True,
                    help="Output directory (created if missing).")
    add_common_flags(sr)


def run(args: argparse.Namespace, output: Output) -> int:
    binary = _resolve_sim_binary(args.bin, args.project)
    if not binary or not binary.exists():
        output.failure(
            exit_code=PROJECT_NOT_FOUND,
            error=(
                "sim binary not found. Pass --bin PATH explicitly, or "
                "--project PROJECT_ROOT pointing at a tree with "
                "sim/build/<binary>. The framework no longer assumes "
                "any default consumer (post G-6 cleanup, May 2026)."
            ),
        )
        return PROJECT_NOT_FOUND

    # Load the project's scenes.json so we can map scene-name → index.
    cfg = _load_scene_config(binary)
    scenes_list = cfg.get("scenes", [])
    scene_tolerances = cfg.get("tolerances", {})
    cfg_default_threshold = cfg.get("default_threshold", DEFAULT_THRESHOLD)

    if args.sim_subcommand == "snapshot":
        ok, err = _run_snapshot(binary, args.scene, args.out, args.ms)
        if not ok:
            output.failure(exit_code=GENERIC_ERROR, error=f"sim: {err}")
            return GENERIC_ERROR
        size = args.out.stat().st_size
        output.success(
            {"scene": args.scene, "out": str(args.out), "size_bytes": size,
             "binary": str(binary)},
            human=f"sim snapshot scene={args.scene} -> {args.out} ({size:,} bytes)",
        )
        return OK

    # diff / update-golden share the same scene-list parsing + golden dir
    # discovery; record takes a single --scene so handles it separately.
    indices: list[tuple[str, int]] = []
    golden_dir = None
    if args.sim_subcommand in ("diff", "update-golden"):
        if not scenes_list:
            output.failure(
                exit_code=PROJECT_NOT_FOUND,
                error=(
                    f"no scenes.json next to {binary} — the project's "
                    "scene name->index map is required for diff/update-golden. "
                    "Create <project>/sim/scenes.json with at minimum: "
                    "{\"scenes\": [\"<your_scene_0>\", ...]}"
                ),
            )
            return PROJECT_NOT_FOUND
        names = [n.strip() for n in args.scenes.split(",") if n.strip()]
        for n in names:
            idx = _scene_index(n, scenes_list)
            if idx is None:
                output.failure(
                    exit_code=GENERIC_ERROR,
                    error=f"unknown scene '{n}'. Known (from scenes.json): {', '.join(scenes_list)}",
                )
                return GENERIC_ERROR
            indices.append((n, idx))
        golden_dir = args.golden if args.golden else _default_golden_dir(binary)

    if args.sim_subcommand == "update-golden":
        golden_dir.mkdir(parents=True, exist_ok=True)
        results: list[dict] = []
        for name, idx in indices:
            out = golden_dir / f"{name}.bmp"
            ok, err = _run_snapshot(binary, idx, out, args.ms)
            results.append({"scene": name, "idx": idx, "ok": ok, "path": str(out), "err": err})
            output.info(f"  {name:10} idx={idx} -> {'OK' if ok else 'FAIL: ' + err}")
        bad = [r for r in results if not r["ok"]]
        if bad:
            output.failure(exit_code=GENERIC_ERROR,
                           error=f"{len(bad)} scene(s) failed", details={"results": results})
            return GENERIC_ERROR
        output.success(
            {"golden_dir": str(golden_dir), "scenes": results, "count": len(results)},
            human=f"refreshed {len(results)} golden snapshots in {golden_dir}",
        )
        return OK

    if args.sim_subcommand == "diff":
        if not golden_dir.exists():
            output.failure(
                exit_code=GENERIC_ERROR,
                error=f"golden dir {golden_dir} does not exist — run `esp-harness sim update-golden` first",
            )
            return GENERIC_ERROR

        with tempfile.TemporaryDirectory(prefix="esp_harness_sim_diff_") as td:
            tmp_dir = Path(td)
            results: list[dict] = []
            failed = []
            for name, idx in indices:
                current = tmp_dir / f"{name}.bmp"
                ok, err = _run_snapshot(binary, idx, current, args.ms)
                if not ok:
                    results.append({"scene": name, "ok": False, "error": err})
                    failed.append(name)
                    continue
                golden = golden_dir / f"{name}.bmp"
                if not golden.exists():
                    results.append({"scene": name, "ok": False, "error": "no golden",
                                    "current": str(current)})
                    failed.append(name)
                    continue
                ratio, note = _bmp_diff_ratio(current, golden, args.pixel_tol)
                if ratio < 0:
                    output.failure(exit_code=GENERIC_ERROR, error=note)
                    return GENERIC_ERROR
                thresh = (args.threshold if args.threshold is not None
                          else scene_tolerances.get(name, cfg_default_threshold))
                passed = ratio <= thresh
                entry = {
                    "scene": name,
                    "diff_ratio": round(ratio, 5),
                    "threshold": thresh,
                    "passed": passed,
                    "note": note,
                }
                results.append(entry)
                if not passed:
                    failed.append(name)
                if args.save_diffs:
                    args.save_diffs.mkdir(parents=True, exist_ok=True)
                    shutil.copy(current, args.save_diffs / f"{name}.current.bmp")
                    shutil.copy(golden,  args.save_diffs / f"{name}.golden.bmp")
                output.info(
                    f"  {name:10} idx={idx:2}  diff={ratio*100:6.3f}%  "
                    f"threshold={thresh*100:.2f}%  {'PASS' if passed else 'FAIL'}"
                )

        payload = {
            "golden_dir": str(golden_dir),
            "scenes": results,
            "threshold": args.threshold,
            "pixel_tol": args.pixel_tol,
            "failed": failed,
        }
        if failed:
            output.failure(
                exit_code=GENERIC_ERROR,
                error=f"{len(failed)} scene(s) regressed: {', '.join(failed)}",
                details=payload,
            )
            return GENERIC_ERROR
        output.success(payload, human=f"all {len(results)} scenes within threshold")
        return OK

    if args.sim_subcommand == "record":
        # Allow scene by name or by numeric index.
        try:
            scene_idx = int(args.scene)
        except ValueError:
            if not scenes_list:
                output.failure(
                    exit_code=PROJECT_NOT_FOUND,
                    error=(
                        f"can't resolve scene name '{args.scene}' — no "
                        f"scenes.json next to {binary}. Use numeric --scene N "
                        "or ship a scenes.json."
                    ),
                )
                return PROJECT_NOT_FOUND
            scene_idx = _scene_index(args.scene, scenes_list)
            if scene_idx is None:
                output.failure(
                    exit_code=GENERIC_ERROR,
                    error=f"unknown scene '{args.scene}'. Known: {', '.join(scenes_list)}",
                )
                return GENERIC_ERROR
        args.out.mkdir(parents=True, exist_ok=True)
        captures: list[dict] = []
        for i in range(args.frames):
            exit_ms = args.settle + args.interval * i
            out = args.out / f"frame_{i:03d}.bmp"
            ok, err = _run_snapshot(binary, scene_idx, out, exit_ms)
            captures.append({"frame": i, "exit_ms": exit_ms,
                              "path": str(out), "ok": ok, "err": err})
            output.info(f"  frame {i:03d}  t={exit_ms:5d}ms  -> {'OK' if ok else 'FAIL: ' + err}")
        bad = [c for c in captures if not c["ok"]]
        if bad:
            output.failure(exit_code=GENERIC_ERROR,
                           error=f"{len(bad)} frame(s) failed",
                           details={"frames": captures})
            return GENERIC_ERROR
        output.success(
            {"scene": args.scene, "frames": captures, "out_dir": str(args.out),
             "count": len(captures)},
            human=f"recorded {len(captures)} frames -> {args.out}",
        )
        return OK

    output.failure(exit_code=GENERIC_ERROR, error=f"unknown sim subcommand: {args.sim_subcommand}")
    return GENERIC_ERROR
