"""`esp-harness sim` — drive the host LVGL simulator.

The companion `esp32-harness-showcase` ships a `sim/` directory that
builds `aurora_sim.exe` (or `aurora_sim` on Linux/macOS) — a native
binary running selected LVGL scenes inside an SDL2 window. This
command wraps that binary so the AI loop can snapshot scenes without
flashing.

Subcommands:
  snapshot     start a scene, run for N ms, dump SDL window to BMP, exit.

Why BMP, not PNG: lets the sim depend only on SDL2 (already a hard
dep). PNG conversion is a host concern — Python has Pillow if you want
it, but most diff tooling handles BMP fine. For visual sharing the
sim README documents a `[System.Drawing]` PowerShell one-liner.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from esp_harness.exit_codes import GENERIC_ERROR, OK, PROJECT_NOT_FOUND
from esp_harness.output import Output


# Index <-> name map for sim binary's register order (sim/main.c).
# Keep in sync with showcase/sim/main.c::scene_fw_register() block.
SIM_SCENES = [
    "halo", "grid", "bloom", "tilt", "pulse", "cell",
    "keys", "tone", "system", "glow", "spin", "notify", "track",
]

# Per-scene diff threshold (max fraction of differing pixels). Used only
# when CLI --threshold is not explicitly given. Animation-driven scenes
# need looser bounds because the phase varies between snapshot runs.
SCENE_TOLERANCES = {
    "pulse": 0.05,   # 3 s breathing ring; phase varies run-to-run
}
DEFAULT_THRESHOLD = 0.01   # tight default for static scenes


def _scene_index(name: str) -> int | None:
    try:
        return SIM_SCENES.index(name)
    except ValueError:
        return None


def _default_golden_dir(binary: Path) -> Path:
    """Golden dir lives at <showcase>/sim/golden/, where <showcase>
    contains the sim/build/aurora_sim.exe we found."""
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


def _default_sim_binary(project: Path | None) -> Path | None:
    """Locate aurora_sim.exe relative to project. project=None means
    walk the monorepo layout — toolkit lives at <root>/tools/esp-harness/,
    Aurora example at <root>/examples/aurora/."""
    candidates: list[Path] = []
    if project:
        candidates.append(project / "sim" / "build" / "aurora_sim.exe")
        candidates.append(project / "sim" / "build" / "aurora_sim")
    # Monorepo guess: <root>/examples/aurora/sim/build/...
    toolkit_root  = Path(__file__).resolve().parents[3]     # tools/esp-harness/
    monorepo_root = toolkit_root.parents[1]                 # esp-harness/
    aurora_sim    = monorepo_root / "examples" / "aurora" / "sim" / "build"
    candidates.append(aurora_sim / "aurora_sim.exe")
    candidates.append(aurora_sim / "aurora_sim")
    # Legacy v1.3 layout — older sibling repo, kept for back-compat during migration.
    legacy_sibling = toolkit_root.parent / "esp32-harness-showcase" / "sim" / "build"
    candidates.append(legacy_sibling / "aurora_sim.exe")
    candidates.append(legacy_sibling / "aurora_sim")
    for c in candidates:
        if c.exists():
            return c
    return None


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser(
        "sim",
        help="Drive the host LVGL simulator (snapshot, etc).",
        description="Wraps the host build of the showcase sim/ binary.",
    )
    p.add_argument(
        "--bin", type=Path, default=None,
        help="Path to aurora_sim.exe (auto-detected via --project or sibling-repo guess).",
    )
    p.add_argument(
        "--project", type=Path, default=None,
        help="Showcase project root (contains sim/build/aurora_sim.exe).",
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
                    help="Comma-separated scene names matching scene_*.c (no scene_ prefix). "
                         "e.g. halo,grid,bloom,tilt,system")
    sd.add_argument("--golden", type=Path, default=None,
                    help="Golden directory (default: <showcase>/sim/golden).")
    sd.add_argument("--ms", type=int, default=500,
                    help="Settle time per scene snapshot.")
    sd.add_argument("--threshold", type=float, default=None,
                    help="Max fraction of differing pixels per scene. If omitted, uses per-scene table (SCENE_TOLERANCES in sim.py); default fallback is 0.01.")
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
                    help="Golden directory (default: <showcase>/sim/golden).")
    su.add_argument("--ms", type=int, default=500)
    add_common_flags(su)

    # ── record ────────────────────────────────────────────────────
    sr = sp.add_parser(
        "record",
        help="Capture a sequence of snapshots across an animation timeline.",
        description=(
            "Spawns aurora_sim N times against the same scene, each with a "
            "progressively longer --exit-after-ms, capturing the animation "
            "at deterministic phase points. Output: <out>/frame_000.bmp, "
            "frame_001.bmp, ...  Use this on animated scenes (Pulse "
            "breathes, Spin gyro-bars wiggle) to review motion."
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
    binary = args.bin or _default_sim_binary(args.project)
    if not binary or not binary.exists():
        output.failure(
            exit_code=PROJECT_NOT_FOUND,
            error=(
                "aurora_sim binary not found. Build it via "
                "esp32-harness-showcase/sim/CMakeLists.txt (see that "
                "directory's README), or pass --bin PATH explicitly."
            ),
        )
        return PROJECT_NOT_FOUND

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
        names = [n.strip() for n in args.scenes.split(",") if n.strip()]
        for n in names:
            idx = _scene_index(n)
            if idx is None:
                output.failure(
                    exit_code=GENERIC_ERROR,
                    error=f"unknown scene '{n}'. Known: {', '.join(SIM_SCENES)}",
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
                          else SCENE_TOLERANCES.get(name, DEFAULT_THRESHOLD))
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
            scene_idx = _scene_index(args.scene)
            if scene_idx is None:
                output.failure(
                    exit_code=GENERIC_ERROR,
                    error=f"unknown scene '{args.scene}'. Known: {', '.join(SIM_SCENES)}",
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
