# Releasing aurora-harness

Process for cutting a new component version + publishing to the
[ESP-IDF Component Registry](https://components.espressif.com/).

## Versioning

`idf_component.yml::version` follows semver (`MAJOR.MINOR.PATCH`):

| Bump | When |
|---|---|
| MAJOR | A breaking public API change. `console_protocol.h`, `scene_framework.h`, `toast.h`, `progress.h`, `default_cmds.h`, `screenshot.h`, `bsp_iface.h`. |
| MINOR | Backwards-compatible additions (new function, new optional struct field, new opt-in command). |
| PATCH | Bug fixes; no API surface change. |

The version in `idf_component.yml` should match the showcase repo's git
tag at release time (we ship them together — Aurora the firmware acts
as the reference release for the component).

## Release checklist

Run from the showcase repo root.

1. **Verify clean state**
   - `git status` shows no uncommitted changes
   - `esp-harness build` succeeds
   - `esp-harness sim diff` 13/13 PASS
   - `esp-harness test` 3/3 PASS

2. **Bump versions** (both must match)
   - `components/aurora-harness/idf_component.yml::version`
   - `CHANGELOG.md` add a `## [X.Y.Z] — YYYY-MM-DD` section

3. **Commit + tag**
   ```bash
   git add components/aurora-harness/idf_component.yml CHANGELOG.md
   git commit -m "release: aurora-harness vX.Y.Z"
   git tag -a vX.Y.Z -m "vX.Y.Z release notes summary"
   git push origin master --tags
   ```

4. **Publish to registry** (one-time setup: get API token at
   https://components.espressif.com/ → profile → API tokens)
   ```bash
   # PowerShell:
   $env:IDF_COMPONENT_API_TOKEN = "<your-token>"
   cd components/aurora-harness
   compote component upload --name aurora-harness --namespace caldis
   ```
   `compote` ships with ESP-IDF's component manager (`pip install
   idf-component-manager` if you don't have it).

5. **Verify consumability**
   - Create a throwaway project: `esp-harness init test-pull`
   - In its `main/idf_component.yml`, depend on the new published version:
     ```yaml
     dependencies:
       caldis/aurora-harness: "^X.Y.Z"
     ```
   - `esp-harness build` — should pull the component from registry
     instead of needing `EXTRA_COMPONENT_DIRS`.

## Currently NOT published

As of v1.4, the component is **only** consumed via path-based
`EXTRA_COMPONENT_DIRS`. The metadata is registry-ready but no upload
has been performed.

## Backwards compatibility policy

Once published, the component holds these contracts:

- **No deletion** of public API in a minor release. Mark deprecated,
  keep working, schedule removal for a major bump.
- **`harness_default_register()` may register more commands** in a
  minor release. Consumers who don't want a particular command should
  register their own subset via `console_protocol_register(&...)`
  rather than calling the helper.
- **`scene_t` struct may grow** (new optional trailing fields) in a
  minor release. Designated-initialiser usage (`{.id = ..., .init =
  ...}`) makes this safe; positional initialisers do not — never
  document them.

## Yanking a bad release

If a release ships with a regression, increment PATCH with the fix
(don't yank). Registry yank is for security holes only.
