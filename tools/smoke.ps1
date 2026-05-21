# tools/smoke.ps1 — pre-release quality gate.
#
# Runs the regression cases that have already caught real bugs once,
# in dependency order (cheap host-only first → expensive device E2E
# last). Stops on first failure with a clear error.
#
# Usage:
#   .\tools\smoke.ps1                     # full suite (needs device)
#   .\tools\smoke.ps1 -SkipDevice         # host-only (CI mode)
#   .\tools\smoke.ps1 -Port COM9          # explicit port
#
# Exit codes: 0=all pass, 1=first failure (see stdout for which).

param(
    [switch]$SkipDevice,
    [string]$Port = "COM9",
    [string]$RepoRoot = "$PSScriptRoot\.."
)

$ErrorActionPreference = "Stop"
Set-Location (Resolve-Path $RepoRoot)
$py = ".\tools\esp-harness\.venv\Scripts\python.exe"
if (-not (Test-Path $py)) {
    Write-Host "FATAL: toolkit venv not found at $py. Run tools/esp-harness/install.ps1 first." -ForegroundColor Red
    exit 2
}

$passed = 0
$failed = @()

function Test-Case {
    param([string]$name, [scriptblock]$body)
    Write-Host -NoNewline ("[{0,-44}] " -f $name)
    try {
        $r = & $body
        if ($r -eq $false) { throw "assertion returned false" }
        Write-Host -ForegroundColor Green "PASS"
        $script:passed++
    } catch {
        Write-Host -ForegroundColor Red ("FAIL: " + $_.Exception.Message)
        $script:failed += "$name :: $_"
    }
}

function Json-Of {
    param([string[]]$argv)
    $out = & $py -m esp_harness @argv --json 2>&1 | Select-Object -Last 1
    return ($out | ConvertFrom-Json)
}

function Console-Body {
    param([string]$cmd, [int]$timeout = 5)
    $line = & $py -m esp_harness console --cmd $cmd --port $Port --raw --timeout $timeout 2>&1 |
            Where-Object { $_ -match "^OK:|^ERR:" } | Select-Object -First 1
    if (-not $line) { throw "no OK/ERR reply within $timeout s" }
    if ($line.StartsWith("ERR:")) { throw $line }
    $body = $line.Substring(4).Trim()
    try { return ($body | ConvertFrom-Json) } catch { return $body }
}

Write-Host ""
Write-Host "── host-only gates ─────────────────────────────────────────────"

Test-Case "doctor 8/8 checks pass" {
    $j = Json-Of @("doctor")
    if ($j.n_ok -ne 8) { throw "expected 8 ok, got $($j.n_ok)" }
    return $true
}

Test-Case "pytest 3/3 integration tests" {
    $out = & $py -m pytest .\tools\esp-harness\tests\ -q 2>&1
    $last = ($out | Select-Object -Last 3) -join " "
    if ($last -notmatch "3 passed") { throw "pytest did not report 3 passed: $last" }
    return $true
}

Test-Case "sim diff 13 scenes identical" {
    $scenes = "halo,grid,bloom,tilt,pulse,cell,keys,tone,system,glow,spin,notify,track"
    $j = Json-Of @("sim","diff","--scenes",$scenes)
    if ($j.failed.Count -ne 0) { throw "$($j.failed.Count) scenes failed" }
    return $true
}

Test-Case "manifest exposes >= 17 toolkit cmds" {
    $j = Json-Of @("manifest","--no-device")
    if ($j.toolkit_commands.Count -lt 17) { throw "only $($j.toolkit_commands.Count) cmds" }
    return $true
}

if ($SkipDevice) {
    Write-Host ""
    Write-Host "── device gates skipped (--SkipDevice) ─────────────────────────"
} else {
    Write-Host ""
    Write-Host "── device gates ────────────────────────────────────────────────"

    Test-Case "device responds to ?ping" {
        $body = Console-Body "?ping"
        if ($body -ne "pong") { throw "want 'pong' got '$body'" }
        return $true
    }
    Test-Case "?stat reports fps > 25" {
        $j = Console-Body "?stat"
        if ($j.fps -lt 25) { throw "fps=$($j.fps) (want > 25)" }
        return $true
    }
    Test-Case "?stat scene_count == 20" {
        $j = Console-Body "?stat"
        if ($j.scene_count -ne 20) { throw "got $($j.scene_count)" }
        return $true
    }
    Test-Case "?sys reports IDF + reset reason" {
        $j = Console-Body "?sys"
        if (-not $j.idf -or -not $j.reset_reason) { throw "missing idf/reset_reason field" }
        return $true
    }
    Test-Case "audio tone reports non-zero bytes (L1 regression)" {
        $j = Console-Body "audio tone 880 200 30" 5
        if ($j.bytes -le 0) { throw "bytes=$($j.bytes) — esp_codec_dev_write return-semantics bug back?" }
        return $true
    }
    Test-Case "audio mic peak in plausible range (L2 regression)" {
        $j = Console-Body "audio mic 800" 5
        if ($j.peak_dbfs -gt -10 -or $j.peak_dbfs -lt -100) {
            throw "peak_dbfs=$($j.peak_dbfs) — ADC throwaway gone?"
        }
        return $true
    }
    Test-Case "?ota info reports running ota_0 or ota_1" {
        $j = Console-Body "?ota"
        if ($j.running.label -notmatch "^ota_[01]$") { throw "running=$($j.running.label) — partitions changed?" }
        return $true
    }
    Test-Case "scene system → ?stat scene_id == system" {
        Console-Body "scene system" | Out-Null
        Start-Sleep -Milliseconds 400
        $j = Console-Body "?stat"
        if ($j.scene_id -ne "system") { throw "got $($j.scene_id)" }
        return $true
    }
    Test-Case "scene halo (return to default)" {
        Console-Body "scene halo" | Out-Null
        return $true
    }
}

Write-Host ""
$total = $passed + $failed.Count
if ($failed.Count -gt 0) {
    Write-Host ("FAILED — {0}/{1} cases" -f $failed.Count, $total) -ForegroundColor Red
    foreach ($f in $failed) { Write-Host ("  • " + $f) -ForegroundColor Red }
    exit 1
} else {
    Write-Host ("PASSED — {0}/{0} cases" -f $passed) -ForegroundColor Green
    exit 0
}
