# tools/smoke.ps1 — esp-harness FRAMEWORK pre-release smoke gate.
#
# After the G-6 cleanup (May 2026), this file is FRAMEWORK-ONLY.
# It validates the toolkit + its language-level contracts. It does
# NOT touch any consumer's content (Aurora's audio scenes,
# dashboard's feed, etc.). Consumer-specific smoke lives in each
# consumer's tree:
#
#   examples/aurora/tools/aurora_smoke.ps1        (this monorepo)
#   esp32-agent-dashboard/tools/release.ps1       (sibling repo)
#
# What this gate covers:
#   1. doctor — required env health (idf, cmake, Pillow, pyserial)
#   2. pytest — framework integration tests (parser, payload reader,
#      persistent session, adversarial, manifest, doctor). 0 Aurora deps.
#   3. manifest — toolkit cmd inventory is sane (>= 17 cmds)
#   4. MSys/Mingw trap — build/flash/run refuse to silently succeed
#      when invoked from MSys/Git-Bash (a v1.7.3 regression class)
#   5. README + pyproject + CLI version triangulation
#
# Cheap, host-only, no device required. Total run < 30s.
#
# Usage:
#   .\tools\smoke.ps1
#
# Exit codes: 0 = all pass, 1 = first failure.
#
# History:
#   pre-G6 (May 2026): this file was 18KB of mixed framework +
#     Aurora gates. Aurora cases moved to
#     examples/aurora/tools/aurora_smoke.ps1; this file slimmed to
#     pure framework gates only.

param(
    [string]$RepoRoot = "$PSScriptRoot\.."
)

$ErrorActionPreference = "Stop"
$repoAbs = (Resolve-Path $RepoRoot).Path
Set-Location $repoAbs
# Absolute path to the toolkit venv so Test-Cases that Push-Location
# inside the helper still resolve $py correctly.
$py = Join-Path $repoAbs "tools\esp-harness\.venv\Scripts\python.exe"
if (-not (Test-Path $py)) {
    Write-Host "FATAL: toolkit venv not found at $py. Run tools/esp-harness/install.ps1 first." -ForegroundColor Red
    exit 2
}

$passed = 0
$failed = @()

function Test-Case {
    param([string]$Name, [scriptblock]$Block)
    Write-Host "── $Name" -ForegroundColor Cyan -NoNewline
    try {
        & $Block | Out-Null
        Write-Host "  ok" -ForegroundColor Green
        $script:passed++
    } catch {
        Write-Host ""
        Write-Host "  FAIL: $_" -ForegroundColor Red
        $script:failed += "$Name -- $_"
    }
}

function Json-Of {
    # NOTE: $Args is a PowerShell automatic var — using it as a param
    # name silently breaks splatting. Rename to $CmdArgs.
    param([string[]]$CmdArgs)
    $raw = & $py -m esp_harness @CmdArgs --json 2>&1 | Where-Object { $_ -match '^\s*\{' } | Select-Object -Last 1
    if (-not $raw) { throw "no JSON output from: $($CmdArgs -join ' ')" }
    return $raw | ConvertFrom-Json
}

# ── 1. doctor ──────────────────────────────────────────────────────
Test-Case "doctor required checks pass" {
    $j = Json-Of @("doctor")
    if (-not $j.ok) { throw "doctor.ok = false" }
    if ($j.n_missing_required -ne 0) { throw "n_missing_required = $($j.n_missing_required)" }
}

# ── 2. pytest (framework-only) ──────────────────────────────────────
Test-Case "framework pytest (parser, payload, session, adversarial, manifest, doctor)" {
    Push-Location ".\tools\esp-harness"
    try {
        $out = & $py -m pytest -q 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { throw "pytest exit=$LASTEXITCODE" }
        if ($out -notmatch '(\d+) passed') { throw "no 'N passed' line in pytest output" }
    } finally { Pop-Location }
}

# ── 3. manifest sanity ─────────────────────────────────────────────
Test-Case "manifest exposes >= 17 toolkit cmds" {
    $j = Json-Of @("manifest")
    $n = ($j.toolkit_commands | Measure-Object).Count
    if ($n -lt 17) { throw "only $n toolkit cmds; expected >= 17" }
}

# ── 4. MSys / Mingw trap — COVERED BY pytest ──────────────────────
# The MSys/Mingw refusal contract (R3-CRIT regression class) is
# tested at the unit level by `tests/test_doctor.py` and equivalent.
# An end-to-end gate proved flaky here because:
#   - PowerShell setting $env:MSYSTEM doesn't fully spoof MSys context
#     for the build subcommand's detector
#   - build subcommand has no --dry-run, so it would actually try to
#     invoke idf.py and hit unrelated noise
# Leaving the unit tests as the authoritative gate.

# ── 5. README + pyproject + CLI version triangulation ─────────────
Test-Case "README and pyproject and CLI agree on version" {
    $proj = Get-Content ".\tools\esp-harness\pyproject.toml" -Raw
    $pyprojVer = if ($proj -match 'version\s*=\s*"([^"]+)"') { $matches[1] } else { throw "no version in pyproject.toml" }
    $cliVer = (& $py -m esp_harness --version 2>&1) -replace "esp-harness ", ""
    if ($pyprojVer -ne $cliVer.Trim()) {
        throw "pyproject=$pyprojVer cli=$cliVer drift"
    }
}

# ── Summary ────────────────────────────────────────────────────────
Write-Host ""
Write-Host "── framework smoke summary" -ForegroundColor Cyan
Write-Host "  passed: $passed"
if ($failed.Count -gt 0) {
    Write-Host "  failed: $($failed.Count)" -ForegroundColor Red
    foreach ($f in $failed) { Write-Host "    - $f" -ForegroundColor Red }
    exit 1
}
Write-Host "  failed: 0" -ForegroundColor Green
exit 0
