# tools/release.ps1 — single-shot release helper.
#
# Implements RELEASING.md's 9-step checklist as one command, aborting
# on the first failure. The defining failure mode (v1.7.2 shipping
# with pyproject still at 1.7.1) is now impossible because step 5
# version-sanity-gates the new version BEFORE step 6 commits.
#
# Usage:
#   .\tools\release.ps1 1.7.4
#   .\tools\release.ps1 1.7.4 -DryRun         # do everything but git push / gh release
#   .\tools\release.ps1 1.7.4 -SkipSmoke      # if you really really know what you're doing
#
# Pre-conditions:
#   - cwd is clean
#   - on master
#   - smoke gate green (auto-checked unless -SkipSmoke)
#
# Run from the repo root.

param(
    [Parameter(Mandatory=$true, Position=0)][string]$Version,
    [string]$ReleaseTitle = "",
    [switch]$DryRun,
    [switch]$SkipSmoke,
    [switch]$SkipPagesBuild
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $repoRoot

function Say($msg) { Write-Host -ForegroundColor Cyan "[release] $msg" }
function Die($msg) {
    Write-Host -ForegroundColor Red "[release] FATAL: $msg"
    exit 1
}

if ($Version -notmatch "^\d+\.\d+\.\d+$") {
    Die "Version must be X.Y.Z (got: $Version)"
}
$tag = "v$Version"
if (-not $ReleaseTitle) {
    $ReleaseTitle = "v$Version"
}

# 0. Sanity — clean tree, on master.
$dirty = (& git status --porcelain) -join ""
if ($dirty) { Die "working tree is dirty. Commit or stash first." }

$branch = & git rev-parse --abbrev-ref HEAD
if ($branch -ne "master") { Die "not on master (on $branch)" }

$existingTag = & git tag --list $tag
if ($existingTag) { Die "tag $tag already exists" }

# 1. Pre-release smoke.
if ($SkipSmoke) {
    Say "smoke skipped (--SkipSmoke)"
} else {
    Say "running smoke gate (.\tools\smoke.ps1)"
    & "$repoRoot\tools\smoke.ps1"
    if ($LASTEXITCODE -ne 0) { Die "smoke gate failed; fix before tagging" }
}

# 2. Bump pyproject.toml::version.
$pyproject = "$repoRoot\tools\esp-harness\pyproject.toml"
$content = Get-Content $pyproject -Raw
# Multiline (?m) so ^ anchors to the start of each line, not just the
# whole document. Without it the literal '^version = ...' never
# matches a Get-Content -Raw blob and release.ps1 aborts at step 2.
$pattern = '(?m)^version\s*=\s*"[^"]*"'
if ($content -notmatch $pattern) {
    Die "couldn't find 'version = ...' line in $pyproject"
}
$content = $content -replace $pattern, "version = `"$Version`""
Set-Content -Path $pyproject -Value $content -NoNewline
Say "bumped pyproject.toml::version to $Version"

# 3. Re-install so importlib.metadata picks up the new number.
$venvPy = "$repoRoot\tools\esp-harness\.venv\Scripts\python.exe"
if (-not (Test-Path $venvPy)) {
    Die "toolkit venv missing at $venvPy — run install.ps1 first"
}
Say "refreshing editable install so --version picks up $Version"
& $venvPy -m pip install -e "$repoRoot\tools\esp-harness[test]" --quiet --force-reinstall --no-deps
if ($LASTEXITCODE -ne 0) { Die "pip install -e failed" }

# 5. Version sanity gate (3 sources must all agree).
$cliVersion = (& $venvPy -m esp_harness --version 2>&1) -join " "
$cliVersion = ($cliVersion -split " ")[-1].Trim()
if ($cliVersion -ne $Version) {
    Die "CLI --version reports '$cliVersion' but pyproject is '$Version'"
}
Say "CLI --version = $cliVersion ✓"

$manifestJson = & $venvPy -m esp_harness manifest --no-device --json 2>&1 | Select-Object -Last 1
$manifestVer = ($manifestJson | ConvertFrom-Json).toolkit_version
if ($manifestVer -ne $Version) {
    Die "manifest toolkit_version reports '$manifestVer' but pyproject is '$Version'"
}
Say "manifest.toolkit_version = $manifestVer ✓"

# 4. CHANGELOG sanity — does the new version have an entry?
$changelog = Get-Content "$repoRoot\CHANGELOG.md" -Raw
if ($changelog -notmatch "## \[$([regex]::Escape($Version))\]") {
    Die "CHANGELOG.md has no '## [$Version]' section — add one before releasing"
}
Say "CHANGELOG entry for [$Version] found ✓"

# 6. Commit.
& git add tools/esp-harness/pyproject.toml CHANGELOG.md
$commitMsg = "release($tag): $ReleaseTitle"
Say "committing: $commitMsg"
& git commit -m $commitMsg
if ($LASTEXITCODE -ne 0) { Die "git commit failed" }

# 7. Tag.
Say "tagging: $tag"
& git tag -a $tag -m $ReleaseTitle

if ($DryRun) {
    Write-Host -ForegroundColor Yellow "[release] DRY RUN — stopping before push. To finalize:"
    Write-Host "  git push origin master"
    Write-Host "  git push origin $tag"
    Write-Host "  gh release create $tag --title `"$ReleaseTitle`" --notes-file <notes.md>"
    exit 0
}

# 7. Push.
Say "pushing master + tag"
& git push origin master
& git push origin $tag

# 8. GitHub Release.
Say "creating GitHub Release"
$notesPlaceholder = "$ReleaseTitle`n`n(See CHANGELOG.md and docs/lessons-v1.7.md for details. Edit this release to expand.)"
& gh release create $tag --title $ReleaseTitle --notes $notesPlaceholder
if ($LASTEXITCODE -ne 0) { Die "gh release create failed" }

# 9. Pages rebuild.
if ($SkipPagesBuild) {
    Say "Pages build skipped (--SkipPagesBuild)"
} else {
    Say "triggering Pages rebuild"
    & gh api -X POST repos/Caldis/esp-harness/pages/builds | Out-Null
}

Write-Host ""
Write-Host -ForegroundColor Green "[release] $tag shipped."
Write-Host "  Release: https://github.com/Caldis/esp-harness/releases/tag/$tag"
Write-Host "  Pages:   https://caldis.github.io/esp-harness/"
Write-Host ""
Write-Host -ForegroundColor Yellow "[release] reminder: schedule a falsification subagent round to check for regressions (Lesson 17)."
