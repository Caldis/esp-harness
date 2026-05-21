# tools/tests — toolkit integration tests

Pytest suite that exercises `esp-harness` end-to-end. Each test invokes
the CLI as a subprocess, parses the JSON result, and asserts. Zero
extra Python deps beyond `pytest` itself — we deliberately don't pull
in `pytest-embedded`, the toolkit already encapsulates the serial side.

## Run

```bash
.venv\Scripts\python.exe -m pytest tools/tests -v
```

Or via the helper command:

```bash
esp-harness test
```

(see `commands/test.py` — same thing, just wraps the pytest invocation
with the venv interpreter automatically.)

## What's tested

| File | What it asserts |
|---|---|
| `test_doctor.py` | `esp-harness doctor` reports all required deps as OK |
| `test_manifest.py` | `esp-harness manifest` lists ≥14 toolkit commands with valid metadata |
| `test_sim_diff.py` | `esp-harness sim diff` on all 13 host-runnable scenes passes against golden |

## When to skip

Tests auto-skip if their prerequisite is missing:

- The sim binary not built → `test_sim_diff.py` skips (`conftest.py::sim_binary` fixture)
- No sibling `esp32-harness-showcase` clone → fixtures skip
- No venv → all skip

So the suite is safe to run in environments where some workflows
aren't set up yet — only the bits that can actually fail surface as
failures.

## Adding a new test

1. Drop a `test_*.py` next to the existing ones.
2. Use the `esp_harness` fixture (`conftest.py`) to invoke the CLI.
3. Assert against the parsed JSON payload, not the human-mode stdout.
4. Update `commands/manifest.py::TOOLKIT_COMMANDS` entry for `test` if
   you change semantics (test discovery, reporting format).

## CI

This suite is invoked by the GitHub Actions workflow at
`esp32-harness-showcase/.github/workflows/sim-diff.yml` after the
sim build step — see that file for the wiring.
