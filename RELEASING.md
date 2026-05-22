# Releasing esp-harness

The release checklist — what every `vX.Y.Z` tag has to go through to
avoid the failure modes captured in `docs/lessons-v1.7.md`.

## TL;DR

```powershell
# from D:\Code\esp-harness
.\tools\release.ps1 1.7.4
```

The script does steps 1-9 below in order, aborting on the first failure.
Run it on a clean working tree on `master`.

If you'd rather drive manually (or you're on Linux/Mac), follow the steps.

---

## 1. Pre-release smoke (must be 21/21)

```powershell
.\tools\smoke.ps1
```

If any case is RED, fix it before tagging. The whole gate is what
"production-ready for this release" means.

## 2. Bump `pyproject.toml::version`

Single source of truth. Bumping this updates `--version`,
`manifest.toolkit_version`, and the registry pin in scaffolded
`idf_component.yml` via `_pinnable_version()`.

```diff
- version = "1.7.3"
+ version = "1.7.4"
```

**This step is the most common skip-and-ship — Lesson 18 in
`docs/lessons-v1.7.md` documents how v1.7.2 shipped reporting as
`1.7.1` because pyproject was never bumped.**

## 3. Re-install the toolkit so `pip` picks up the new version

```powershell
.\tools\esp-harness\install.ps1 -NoProfileEdit
```

The `importlib.metadata` lookup that backs `__version__` reads the
installed-package metadata. Without a re-install, `--version` lies.

## 4. CHANGELOG entry

Add a new section at the top, ABOVE the previous release. Format
matches the rest of the file. **Don't reuse a "Status pending"
placeholder from the previous release — clean it up first.**

## 5. Sanity-gate the version

```powershell
esp-harness --version
# must match the new version exactly
```

Plus from a fresh PowerShell window (so the venv reactivates):

```powershell
esp-harness manifest --no-device --json | python -c "import sys, json; print(json.load(sys.stdin)['toolkit_version'])"
# must match the new version exactly
```

## 6. Commit + tag

```powershell
git add tools/esp-harness/pyproject.toml CHANGELOG.md
# plus any other release-prep changes
git commit -m "release(vX.Y.Z): <one-line summary>"
git tag -a vX.Y.Z -m "<one-line summary>"
```

## 7. Push

```powershell
git push origin master
git push origin vX.Y.Z
```

## 8. GitHub Release

```powershell
gh release create vX.Y.Z --title "vX.Y.Z — <title>" --notes "..."
```

Notes template mirrors the CHANGELOG entry but rendered as Markdown
for the Releases page.

## 9. Trigger Pages rebuild

```powershell
gh api -X POST repos/Caldis/esp-harness/pages/builds
```

The legacy `docs/`-on-master Pages builder doesn't auto-detect a push;
manual trigger is required.

---

## Skip-list — do NOT do these

- Do **not** edit `__init__.py::__version__` directly. It's computed
  from `pyproject.toml` via `importlib.metadata` at runtime (since
  v1.7.1 / Lesson 14). Hand-maintaining it is what drifted to `1.5.0`
  in the first place.
- Do **not** create the git tag without bumping pyproject first.
  See Lesson 18.
- Do **not** ship a release with a critical fix without running the
  smoke gate in BOTH PowerShell AND Git Bash (the Lesson 15
  cross-shell trap surfaces only in Git Bash).
- Do **not** rely on the verify-mode adversarial subagent rounds
  alone — schedule at least one falsification round per release
  cycle. See Lesson 17.

## What to do after release

- Watch the Pages build for ~1 min (`gh api repos/Caldis/esp-harness/pages/builds/latest`)
  until `status: built`. If it fails, debug from the GitHub Pages
  status link in the API response.
- If this is a `vX.Y.Z` where `Z>0` (a patch with critical fixes),
  consider running an immediate falsification subagent round —
  the patch may have re-introduced an adjacent regression.
