# sim/tools/setup.ps1 — detect prerequisites for the host build.
#
# Does NOT install anything automatically — installing dev libs is a
# system-wide change and should be the user's explicit choice. This
# script just probes and prints the install commands you'd run.

Write-Host "Aurora simulator prerequisite check"
Write-Host "===================================="
Write-Host ""

# CMake
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    # Try ESP-IDF EIM install
    $eim = Get-ChildItem "C:\Espressif\tools\cmake\*\bin\cmake.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($eim) { $cmake = @{ Source = $eim.FullName } }
}
if ($cmake) {
    Write-Host "[OK]   cmake at $($cmake.Source)"
} else {
    Write-Host "[MISS] cmake — install via:"
    Write-Host "         choco install cmake -y"
}

# C compiler
$gcc = Get-Command gcc -ErrorAction SilentlyContinue
$cl  = Get-Command cl  -ErrorAction SilentlyContinue
if ($gcc) {
    Write-Host "[OK]   gcc at $($gcc.Source)"
} elseif ($cl) {
    Write-Host "[OK]   cl at $($cl.Source) (MSVC)"
} else {
    Write-Host "[MISS] no host C compiler — install one of:"
    Write-Host "         choco install mingw -y       (MinGW gcc)"
    Write-Host "         (or install Visual Studio Build Tools manually)"
}

# SDL2
$sdl_paths = @(
    "C:\ProgramData\chocolatey\lib\sdl2",
    "C:\SDL2",
    "$env:VCPKG_ROOT\installed\x64-windows\include\SDL2"
)
$sdl_found = $null
foreach ($p in $sdl_paths) {
    if ($p -and (Test-Path $p)) { $sdl_found = $p; break }
}
if ($sdl_found) {
    Write-Host "[OK]   SDL2 located at $sdl_found"
} else {
    Write-Host "[MISS] SDL2 — install via one of:"
    Write-Host "         choco install sdl2 -y"
    Write-Host "         (or vcpkg / manual download — see sim/README.md)"
}

# LVGL
$lvgl = "$PSScriptRoot\..\..\managed_components\lvgl__lvgl\lvgl.h"
if (Test-Path $lvgl) {
    Write-Host "[OK]   LVGL at managed_components/lvgl__lvgl/"
} else {
    Write-Host "[MISS] LVGL — run an ESP-IDF build first to populate it:"
    Write-Host "         esp-harness build --project ..\..\"
}

Write-Host ""
Write-Host "When all four are [OK]:"
Write-Host "   cd sim"
Write-Host "   cmake -B build -G `"MinGW Makefiles`""
Write-Host "   cmake --build build"
Write-Host "   .\build\aurora_sim.exe"
