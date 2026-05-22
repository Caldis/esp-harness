# sim/golden — visual regression baseline

These BMPs are the reference output for `esp-harness sim diff`. They
were captured from a known-good build of `aurora_sim.exe` running
each scene for 500 ms then doing `SDL_SaveBMP`.

## When to refresh

After an intentional UI change (typography, color, layout) that
affects one or more scenes, regenerate the affected entries:

```powershell
esp-harness sim update-golden --scenes halo,system    # or all
```

then commit the new BMPs alongside the source change so future runs
have the right baseline.

## When NOT to refresh

If a diff fails *unexpectedly*, do **not** reflexively re-run
`update-golden` — that just rebases the regression away. Inspect the
diff first:

```powershell
esp-harness sim diff --scenes halo --save-diffs D:\tmp\halo-diff
# inspect D:\tmp\halo-diff\halo.current.bmp vs ...golden.bmp
```

## Per-scene tolerances

Animated scenes need looser thresholds. The defaults live in
`tools/esp-harness/src/esp_harness/commands/sim.py::SCENE_TOLERANCES`:

| Scene | Threshold | Why |
|---|---|---|
| pulse | 5.00% | 3 s breathing ring; phase varies between runs |
| (others) | 1.00% | static UI; should diff at ~0% |

To override at the CLI: `esp-harness sim diff --scenes ... --threshold 0.02`.
