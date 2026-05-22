# esp-harness installer (Windows / PowerShell)
#
# - Creates a dedicated venv at .\.venv
# - Installs the package in editable mode (so future edits to src/ take effect)
# - Adds an `esp-harness` function to the user's PowerShell profile, pointing
#   at the venv's entry-point script.
#
# Re-run safe: idempotent.

[CmdletBinding()]
param(
    [string]$Python = $null,
    [switch]$NoProfileEdit
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$venv = Join-Path $root '.venv'

# ── 1. Pick a Python ─────────────────────────────────────────────────
function Find-Python {
    param([string]$Hint)
    if ($Hint) { return $Hint }
    foreach ($candidate in @(
        'C:\Python314\python.exe',
        'C:\Python312\python.exe',
        "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe",
        "$env:LOCALAPPDATA\Programs\Python\Python311\python.exe",
        "$env:LOCALAPPDATA\Programs\Python\Python310\python.exe"
    )) {
        if (Test-Path $candidate) { return $candidate }
    }
    $py = (Get-Command python -ErrorAction SilentlyContinue).Source
    if ($py) { return $py }
    throw "No Python found. Pass -Python C:\path\to\python.exe"
}

$pythonExe = Find-Python -Hint $Python
Write-Host "[esp-harness] using $pythonExe" -ForegroundColor Cyan

# ── 2. Create venv ───────────────────────────────────────────────────
if (-not (Test-Path $venv)) {
    Write-Host "[esp-harness] creating venv at $venv" -ForegroundColor Cyan
    & $pythonExe -m venv $venv
    if ($LASTEXITCODE -ne 0) { throw "venv creation failed" }
} else {
    Write-Host "[esp-harness] reusing existing venv at $venv" -ForegroundColor DarkGray
}

$venvPython = Join-Path $venv 'Scripts\python.exe'
$venvHarness = Join-Path $venv 'Scripts\esp-harness.exe'

# ── 3. Install package (editable) ────────────────────────────────────
Write-Host "[esp-harness] upgrading pip + installing package..." -ForegroundColor Cyan
& $venvPython -m pip install --upgrade pip --quiet
if ($LASTEXITCODE -ne 0) { throw "pip upgrade failed" }
# Install with the [test] extras so `esp-harness test` works
# out-of-the-box and `tools/smoke.ps1` reaches its full 21/21
# (without test extras, pytest is missing and smoke gates at 20/21).
# Round-4 subagent flagged the gap: fresh-clone smoke was 19/20.
& $venvPython -m pip install -e "$root[test]" --quiet
if ($LASTEXITCODE -ne 0) { throw "package install failed" }

if (-not (Test-Path $venvHarness)) {
    throw "Install completed but $venvHarness not found"
}
Write-Host "[esp-harness] entry-point ready: $venvHarness" -ForegroundColor Green

# ── 4. Smoke test ────────────────────────────────────────────────────
Write-Host "[esp-harness] smoke test..." -ForegroundColor Cyan
& $venvHarness --version
if ($LASTEXITCODE -ne 0) { throw "Smoke test failed" }

# ── 5. Wire into user profiles ───────────────────────────────────────
if (-not $NoProfileEdit) {
    $profiles = @(
        "$HOME\Documents\PowerShell\Microsoft.PowerShell_profile.ps1",
        "$HOME\Documents\WindowsPowerShell\Microsoft.PowerShell_profile.ps1"
    )

    $marker  = '# >>> esp-harness shim'
    $endMark = '# <<< esp-harness shim'
    $block = @"
$marker
function esp-harness { & '$venvHarness' @args }
$endMark
"@

    foreach ($p in $profiles) {
        $dir = Split-Path $p -Parent
        if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
        if (-not (Test-Path $p)) { New-Item -ItemType File -Path $p -Force | Out-Null }

        $content = Get-Content $p -Raw -ErrorAction SilentlyContinue
        if ($null -eq $content) { $content = '' }

        if ($content -match [regex]::Escape($marker)) {
            # replace existing block
            $pattern = [regex]::Escape($marker) + '.*?' + [regex]::Escape($endMark)
            $new = [regex]::Replace($content, $pattern, [regex]::Escape($block) -replace '\\(.)','$1', 'Singleline')
            # the above is fragile; simpler: rebuild
            $lines = Get-Content $p
            $out = @()
            $skip = $false
            foreach ($ln in $lines) {
                if ($ln -match [regex]::Escape($marker)) { $skip = $true; continue }
                if ($skip -and $ln -match [regex]::Escape($endMark)) { $skip = $false; continue }
                if (-not $skip) { $out += $ln }
            }
            $out += ''
            $out += $block.Split("`n")
            Set-Content -Path $p -Value $out -Encoding UTF8
            Write-Host "[esp-harness] updated shim in $p" -ForegroundColor Green
        } else {
            Add-Content -Path $p -Value "`n$block"
            Write-Host "[esp-harness] added shim to $p" -ForegroundColor Green
        }
    }
}

Write-Host ""
Write-Host "Done. Open a NEW PowerShell window and try:" -ForegroundColor Cyan
Write-Host "    esp-harness --version" -ForegroundColor White
Write-Host "    esp-harness port list" -ForegroundColor White
Write-Host "    esp-harness port detect" -ForegroundColor White
