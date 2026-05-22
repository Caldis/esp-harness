# tools/esp-harness/tests — toolkit integration tests

Pytest suite that exercises `esp-harness` end-to-end. Each test invokes
the CLI as a subprocess, parses the JSON result, and asserts.

## Run

From the repo root (Windows / PowerShell — adjust paths for Unix):

```bash
tools/esp-harness/.venv/Scripts/python.exe -m pytest tools/esp-harness/tests -v
```

Or via the helper command (works on any platform once the toolkit is
installed):

```bash
esp-harness test
```

(See `commands/test.py` — same thing, just wraps the pytest invocation
with `sys.executable` automatically. It also checks pytest is
installed and gives an install hint if not.)

## What's tested

| File | What it asserts |
|---|---|
| `test_doctor.py` | `esp-harness doctor` reports all required deps as OK |
| `test_manifest.py` | `esp-harness manifest` lists ≥14 toolkit commands with valid metadata |
| `test_sim_diff.py` | `esp-harness sim diff` on all 13 host-runnable scenes passes against golden |

## When to skip

Tests auto-skip if their prerequisite is missing:

- The sim binary not built → `test_sim_diff.py` skips
  (`conftest.py::sim_binary` fixture)
- No `examples/aurora/` directory → Aurora-dependent fixtures skip
- No toolkit venv → all skip (the `esp_harness` fixture checks for
  `.venv/Scripts/python.exe`)

So the suite is safe to run in environments where some workflows
aren't set up yet — only the bits that can actually fail surface as
failures.

## Adding a new test

1. Drop a `test_*.py` next to the existing ones.
2. Use the `esp_harness` fixture (`conftest.py`) to invoke the CLI.
3. Assert against the parsed JSON payload, not the human-mode stdout.
4. Update `commands/manifest.py::TOOLKIT_COMMANDS` entry for `test`
   if you change semantics (test discovery, reporting format).

## CI

This suite is invoked by the GitHub Actions workflow at
[`.github/workflows/sim-diff.yml`](../../../.github/workflows/sim-diff.yml)
after the sim build step — see that file for the wiring. The CI
installs the toolkit with the `[test]` extras so pytest is in the
venv.

## See also

For the *full* pre-release gate that wraps these integration tests
plus host smoke + device E2E, see [`tools/smoke.ps1`](../../smoke.ps1)
and [`tools/smoke.sh`](../../smoke.sh). The smoke gate is what
gates v1.7.x → v1.7.x+1 promotions.
