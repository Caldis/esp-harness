# Contributing to esp-harness

Thanks for considering a contribution. This doc covers the practical
mechanics; the **why** behind the design lives in
[`docs/manifesto.md`](../docs/manifesto.md).

## Before you start

Please skim, in this order:

1. [`README.md`](../README.md) — what the project is
2. [`docs/manifesto.md`](../docs/manifesto.md) — what we believe + what we won't do
3. [`AGENT.md`](../AGENT.md) — where things go (the file-placement decision table)
4. The README of whichever directory you're about to touch

If your idea is in the "we won't do" list of the manifesto, you'll save
yourself time by raising it as a discussion issue first.

## Setup

```bash
git clone https://github.com/Caldis/esp-harness
cd esp-harness

# Install the toolkit in editable mode
pip install -e tools/esp-harness/[test]

# Verify environment health
esp-harness doctor

# Run the integration suite — should be 3/3 PASS in ≤ 15 s
esp-harness test
```

If `doctor` flags anything, fix it before proceeding (each row has an
install hint).

## Development loop

The shortest cycle is:

```bash
# 1. Edit
vim examples/aurora/main/scenes/scene_foo.c

# 2. Sim-iterate (3 s build + 5 s diff)
cd examples/aurora/sim
cmake --build build -j
esp-harness sim diff --scenes foo

# 3. If a golden legitimately changed:
esp-harness sim update-golden --scenes foo

# 4. Only then flash to the device:
cd ../..
esp-harness build && esp-harness flash
```

For toolkit-side changes:

```bash
# edit tools/esp-harness/src/esp_harness/commands/<x>.py
esp-harness test   # 3 integration tests
```

## What we accept

We're keen on:

- **Bug fixes** with a reproducer (ideally a test case in `tools/esp-harness/tests/`)
- **New examples** under `examples/<name>/` that exercise the harness on a new board / use-case
- **New BSPs** under `boards/<underscored_name>/` for boards you've actually flashed
- **Docs improvements** at any level — particularly clarifications, FAQ additions, troubleshooting entries
- **Reusable primitives** in `components/aurora-harness/` if they pass the "every consumer wants this" test
- **Toolkit commands** that add a genuinely new capability (not "yet another wrapper of `idf.py`")

We're cautious about:

- **Application-specific code in the component.** If it references Aurora's peripherals, scenes, or chrome, it belongs in `examples/aurora/`, not `components/aurora-harness/`.
- **Backwards-compat shims.** Semver bumps are honest. Don't add adapter layers; do bump the version.
- **PlatformIO migration.** We're staying ESP-IDF-native. PIO compatibility *layer* is welcome; PIO replacement is not in scope.
- **Heavy new dependencies.** Each `pip install` line adds friction for users. Justify the cost.

## Branching + PR

```bash
git checkout -b feat/<short-name>      # or fix/<...>, docs/<...>
# ... commits ...
git push origin feat/<short-name>
gh pr create --fill                    # the PR template will load
```

PR title format: `<type>: <imperative summary>`

Examples:
- `feat(component): add harness_modal() overlay primitive`
- `fix(sim): SDL2 link order on macOS x86_64`
- `docs(architecture): clarify Layer 2 vs Layer 3 boundary`
- `chore(ci): bump action versions`

## CI / verification

Every PR runs `.github/workflows/sim-diff.yml` automatically:

1. `apt install libsdl2-dev cmake build-essential`
2. Clone LVGL 9.4
3. Build `examples/aurora/sim/` for Ubuntu
4. `esp-harness sim diff` against committed golden BMPs
5. `pytest tools/esp-harness/tests/`

If your PR breaks the visual regression, either fix the regression or
commit refreshed golden BMPs alongside the source change. (Use
`esp-harness sim update-golden --scenes ...` and commit the result.)

## Code style

- **C**: follow the surrounding code — 4-space indent, no tabs, K&R braces. We don't have a formatter committed; manual care.
- **Python**: PEP 8 with 4-space indent. Type hints on public APIs. We don't run black / ruff in CI yet (might add).
- **Markdown**: prose, not bullet-grids when you can help it. Aim for 80-char lines.
- **Commit messages**: imperative subject (≤ 70 chars), blank line, body explaining *why*.

## What good looks like in a PR

A PR is "ready to review" when it has:

- [ ] One conceptual change. Bundle related cleanups, but a PR fixing a
      bug *and* adding a feature is two PRs.
- [ ] A description explaining the **why**, not just the what.
- [ ] CI green. Local `esp-harness test` green.
- [ ] If touching the component's public API: a CHANGELOG entry + a
      version bump in `components/aurora-harness/idf_component.yml`.
- [ ] If adding a toolkit command: registered in `manifest.py::TOOLKIT_COMMANDS`
      (the lint will fail otherwise).
- [ ] If touching the device console surface: `?help json` still
      enumerates everything (`esp-harness manifest --json | jq .device.commands`).

## Reporting bugs

Use [Issues → Bug Report](https://github.com/Caldis/esp-harness/issues/new?template=bug.md).
Always include:

- `esp-harness --version`
- `esp-harness doctor` output
- ESP-IDF version (`idf.py --version` or `idf-version`)
- The exact command + the exact output

## Asking questions

If it's about "how do I do X with esp-harness?", use
[Issues → Question](https://github.com/Caldis/esp-harness/issues/new?template=question.md).
Public questions help everyone.

If it's "should we do X?", use a Discussion (or open a draft PR with a
sketch).

## Maintainer roles

This is a small project. Today maintainership is:

| Role | People |
|---|---|
| Lead | [@Caldis](https://github.com/Caldis) |
| AI agent contributions | Anthropic Claude, with human review |

## License

By contributing, you agree your contributions are licensed under MIT
(matching the repo). No CLA required.
