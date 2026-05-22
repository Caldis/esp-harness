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

Test-Case "build/flash/run refuse MSys/Mingw exit-0 (R3-CRIT + v1.7.3 regression)" {
    # Round-3 caught the trap in `build`; v1.7.3-prep extended the
    # same fail-closed contract to `flash` and `run`. All three
    # subcommands must reject Git Bash's MSys env so AI agents
    # can't flash stale binaries silently.
    # Find Git Bash specifically (not WSL bash). PowerShell's
    # `Get-Command bash` on a Windows box with WSL installed returns
    # the WSL stub at /usr/bin/bash, which has a totally different
    # behaviour (no MSys env, won't trigger the idf.py refusal).
    $gitBash = $null
    foreach ($candidate in @(
        "C:\Program Files\Git\bin\bash.exe",
        "C:\Program Files (x86)\Git\bin\bash.exe",
        "$env:LOCALAPPDATA\Programs\Git\bin\bash.exe"
    )) {
        if (Test-Path $candidate) { $gitBash = $candidate; break }
    }
    if (-not $gitBash) {
        Write-Host -NoNewline "(skipped: Git Bash not installed) "
        return $true
    }
    $aurora = "D:\Code\esp-harness\examples\aurora"
    # Resolve to absolute path before swapping separators — $py is
    # relative to the smoke script's cwd, Git Bash will cd elsewhere.
    $pyAbs = (Resolve-Path $py).Path
    $pyPosix = $pyAbs.Replace('\','/')
    foreach ($sub in @("build", "flash --port COM9", "run --port COM9 --seconds 2")) {
        $cmd = "cd '$aurora' && '$pyPosix' -m esp_harness $sub --project . --json"
        $out = & $gitBash -lc $cmd 2>&1 | Select-Object -Last 1
        try {
            $j = $out | ConvertFrom-Json
        } catch {
            throw "[$sub] couldn't parse output: $out"
        }
        if ($j.ok -ne $false) { throw "[$sub] expected ok=false, got $($j.ok)" }
        $trig = $j.trigger
        if (-not $trig) { $trig = $j.details.trigger }
        if (-not ($trig -match "MSys|Mingw")) {
            throw "[$sub] no MSys/Mingw trigger in response: $($j.error)"
        }
    }
    return $true
}

Test-Case "version triangulation (--version == manifest != 1.5.0)" {
    $cli_ver = (& $py -m esp_harness --version 2>&1) -join " " | ForEach-Object { ($_ -split " ")[-1] }
    $man = Json-Of @("manifest","--no-device")
    if ($cli_ver -ne $man.toolkit_version) {
        throw "cli=$cli_ver != manifest=$($man.toolkit_version)"
    }
    if ($cli_ver -eq "1.5.0") {
        throw "stuck on stale 1.5.0 — __init__.py drifted from pyproject again?"
    }
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
    Test-Case "tap --wait-evt captures tap_hit (L9 regression)" {
        # Use the toolkit directly to keep the assertion JSON-shaped.
        Console-Body "scene halo" | Out-Null
        Start-Sleep -Milliseconds 300
        $j = & $py -m esp_harness console --cmd "tap 233 233" --port $Port `
              --wait-evt "^tap_hit" --evt-timeout 2 --json 2>&1 |
              Select-Object -Last 1 | ConvertFrom-Json
        if (-not $j.evt_matched) {
            throw "tap_hit EVT not captured (matched_evt=$($j.matched_evt))"
        }
        return $true
    }
    Test-Case "scene next --wait-evt captures pre-ack EVT (R2-bug regression)" {
        Console-Body "scene halo" | Out-Null
        Start-Sleep -Milliseconds 300
        # scene_changed EVT fires DURING cmd_scene's call to scene_fw_show,
        # i.e. before the OK: reply. Round-2 subagent caught the original
        # ack_seen gate dropping these on the floor.
        $j = & $py -m esp_harness console --cmd "scene next" --port $Port `
              --wait-evt "scene_changed" --evt-timeout 2 --json 2>&1 |
              Select-Object -Last 1 | ConvertFrom-Json
        if (-not $j.evt_matched) {
            throw "scene_changed EVT (pre-ack) not captured — pre-ack gate back?"
        }
        return $true
    }
    Test-Case "manifest.device.available is a real bool (R2-bug regression)" {
        # `dm.fetched_ok = X and (list or list)` returns the operand —
        # round-2 subagent saw an 18-element command list leak through
        # as `device.available`. Re-test with a connected device.
        $j = & $py -m esp_harness manifest --port $Port --json 2>&1 |
             Select-Object -Last 1 | ConvertFrom-Json
        $a = $j.device.available
        if ($a -isnot [bool]) {
            throw "device.available is not a bool: type=$($a.GetType().Name) value=$a"
        }
        if ($a -ne $true) {
            throw "device available, but value=false (device queried correctly?)"
        }
        return $true
    }
    Test-Case "--wait-evt no-match returns timed-out evt_wait_ms (R4-edge)" {
        # No-match path: regex matches nothing → resp.matched_evt is
        # null but evt_wait_ms must reflect the wait duration so AI
        # agents can distinguish 'instant match' (0ms) from 'timed
        # out' (≈ evt_timeout). Pre-fix: both were 0 ms.
        $j = & $py -m esp_harness console --cmd "?ping" --port $Port `
              --wait-evt "DEFINITELY_NEVER_HAPPENS" --evt-timeout 1 `
              --json 2>&1 | Select-Object -Last 1 | ConvertFrom-Json
        if ($j.evt_matched) { throw "spurious match against impossible regex" }
        if ($j.evt_wait_ms -lt 800 -or $j.evt_wait_ms -gt 1500) {
            throw "evt_wait_ms=$($j.evt_wait_ms) — expected ~1000ms for --evt-timeout 1"
        }
        return $true
    }
    Test-Case "?keys press boot synth (R3-bug regression)" {
        # Round-3 subagent flagged: no synthetic-keypress means AI
        # agents can't exercise button-gated flows. We added
        # ?keys press <name> [hold_ms] in v1.7.2. Verify: count
        # increments, mid-hold pressed=true, post-release =false.
        #
        # hold_ms = 1500 — three separate `?keys` round-trips at
        # ~200ms each plus margin must complete inside the window,
        # otherwise the override expires before mid-hold query lands.
        $before = (Console-Body "?keys").boot.count
        & $py -m esp_harness console --cmd "?keys press boot 1500" `
              --port $Port --wait-evt "key_press" --evt-timeout 2 `
              --json 2>&1 | Out-Null
        $mid = Console-Body "?keys"
        if (-not $mid.boot.pressed) {
            throw "mid-hold pressed=false (keys_task overrode synth — override window expired before query)"
        }
        if ($mid.boot.count -ne ($before + 1)) {
            throw "count didn't increment: was $before, now $($mid.boot.count)"
        }
        Start-Sleep -Milliseconds 1800
        $after = Console-Body "?keys"
        if ($after.boot.pressed) {
            throw "release didn't fire — synth window stuck"
        }
        return $true
    }
    Test-Case "bench --compare produces structured diff (regressions allowed)" {
        # Doesn't gate on regressions=0 — heap_free / psram_free drift
        # several percent depending on what's run recently (audio
        # loopback alone allocates ~440 KB PSRAM transiently). What we
        # ARE checking: the comparison runs, produces a structured
        # diff, and the baseline isn't a raw git SHA from the v1.5 era.
        $line = & $py -m esp_harness bench --compare --quick --port $Port --json 2>&1 |
                Select-Object -Last 1
        $j = $line | ConvertFrom-Json
        # On regression-failure exit, the compare object is nested
        # under snapshot.compare. On success, it's at top level.
        $cmp = if ($j.compare) { $j.compare } else { $j.snapshot.compare }
        if (-not $cmp) { throw "no 'compare' object anywhere in bench output" }
        if (-not $cmp.diffs -or $cmp.diffs.Count -lt 3) {
            throw "compare.diffs too small: $($cmp.diffs.Count) metrics"
        }
        if ($cmp.baseline_device.app_version -match '^[0-9a-f]+-dirty$') {
            throw "baseline is a raw SHA — needs --baseline against current build"
        }
        if (-not $cmp.baseline_device.app_version.StartsWith("v")) {
            throw "baseline app_version doesn't look like a release tag: $($cmp.baseline_device.app_version)"
        }
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
