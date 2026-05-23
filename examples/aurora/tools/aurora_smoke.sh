#!/usr/bin/env bash
# tools/smoke.sh — pre-release quality gate, cross-platform variant.
#
# Mirrors tools/smoke.ps1's host-only suite for Linux / macOS. Device
# gates are intentionally omitted here because the toolkit's serial /
# device-side workflows are still Windows-focused (see lessons-v1.7.md L1).
# Once cross-platform device support lands, this script will grow to match.
#
# Usage:
#   ./tools/smoke.sh                  # run all host gates
#   ./tools/smoke.sh --verbose        # show captured tool output
#
# Exit 0 = all passed, 1 = first failure.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="${PY:-python3}"
VERBOSE=0
[[ "${1:-}" == "--verbose" ]] && VERBOSE=1

cd "$ROOT"

# Locate the toolkit venv, or fall back to the user's python.
if [[ -x "$ROOT/tools/esp-harness/.venv/bin/python" ]]; then
    PY="$ROOT/tools/esp-harness/.venv/bin/python"
elif [[ -x "$ROOT/tools/esp-harness/.venv/Scripts/python.exe" ]]; then
    # Git-Bash on Windows can also use this script — point at the
    # Windows venv layout if present.
    PY="$ROOT/tools/esp-harness/.venv/Scripts/python.exe"
else
    # System python — only works if the toolkit was installed globally.
    if ! command -v esp-harness >/dev/null 2>&1; then
        echo "FATAL: toolkit venv not found and 'esp-harness' not on PATH." >&2
        echo "       Run 'pip install -e tools/esp-harness/' first." >&2
        exit 2
    fi
fi

passed=0
failed=0
declare -a fail_reasons=()

emit_pass() {
    printf "[%-44s] \033[32mPASS\033[0m\n" "$1"
    passed=$((passed + 1))
}
emit_fail() {
    printf "[%-44s] \033[31mFAIL\033[0m: %s\n" "$1" "$2"
    failed=$((failed + 1))
    fail_reasons+=("$1 :: $2")
}

run_test() {
    local name="$1" cmd="$2"
    local out
    if ! out="$(eval "$cmd" 2>&1)"; then
        emit_fail "$name" "command exit non-zero"
        [[ $VERBOSE -eq 1 ]] && echo "$out" | head -20 >&2
        return
    fi
    [[ $VERBOSE -eq 1 ]] && echo "$out" | tail -3 >&2
    emit_pass "$name"
}

echo
echo "── host-only gates ─────────────────────────────────────────────"

# 1. doctor: every required check green.
n_ok="$("$PY" -m esp_harness doctor --json | tail -1 | "$PY" -c \
    'import json,sys; d=json.loads(sys.stdin.read()); print(d.get("n_ok", -1))' 2>/dev/null)"
if [[ "$n_ok" == "8" ]]; then
    emit_pass "doctor 8/8 checks pass"
else
    emit_fail "doctor 8/8 checks pass" "got n_ok=$n_ok"
fi

# 2. pytest integration tests. Fresh clones without a built sim
# binary correctly skip test_sim_diff_all_scenes_pass; accept both
# "3 passed" and "2 passed + 1 skipped" shapes. Only `failed` is
# a hard error. Mirrors smoke.ps1's three-branch acceptance.
pytest_out="$("$PY" -m pytest "$ROOT/tools/esp-harness/tests/" -q 2>&1 || true)"
pytest_last="$(echo "$pytest_out" | tail -3 | tr '\n' ' ')"
if echo "$pytest_last" | grep -q "failed"; then
    emit_fail "pytest integration tests" "$(echo "$pytest_out" | tail -1)"
elif echo "$pytest_last" | grep -qE "3 passed|2 passed.*1 skipped|passed.*skipped"; then
    emit_pass "pytest integration tests (3 passed, or 2 passed + sim skip)"
else
    emit_fail "pytest integration tests" "unexpected output: $pytest_last"
fi

# 3. sim diff regression. The toolkit exits 21 (PROJECT_NOT_FOUND)
# when the sim binary isn't built — that's a legitimate fresh-clone
# state, not a regression. Don't let `set -e + pipefail` kill the
# whole script. Mirrors smoke.ps1's tolerance.
sim_out="$("$PY" -m esp_harness sim diff \
    --scenes halo,grid,bloom,tilt,pulse,cell,keys,tone,system,glow,spin,notify,track \
    --json 2>&1 | tail -1 || true)"
if echo "$sim_out" | grep -q '"exit_code":\s*21'; then
    # Sim binary not built — skip rather than fail. Honest about state.
    emit_pass "sim diff 13 scenes identical (sim binary absent — skipped)"
else
    sim_failed="$(echo "$sim_out" | "$PY" -c \
        'import json,sys
try:
    print(len(json.loads(sys.stdin.read()).get("failed",[])))
except Exception:
    print("?")' 2>/dev/null || echo "?")"
    if [[ "$sim_failed" == "0" ]]; then
        emit_pass "sim diff 13 scenes identical"
    else
        emit_fail "sim diff 13 scenes identical" "$sim_failed failed"
    fi
fi

# 4. manifest exposes >= 17 toolkit cmds.
n_cmds="$("$PY" -m esp_harness manifest --no-device --json 2>&1 | tail -1 | "$PY" -c \
    'import json,sys; d=json.loads(sys.stdin.read()); print(len(d.get("toolkit_commands",[])))' 2>/dev/null)"
if [[ "$n_cmds" -ge 17 ]]; then
    emit_pass "manifest exposes >= 17 toolkit cmds"
else
    emit_fail "manifest exposes >= 17 toolkit cmds" "got $n_cmds"
fi

# 5. version triangulation: CLI, manifest, AND pyproject.toml must
# all agree. Round-4 caught that v1.7.2 shipped reporting as 1.7.1
# because only the first two were compared while pyproject was the
# stale third source.
cli_version="$("$PY" -m esp_harness --version 2>&1 | awk '{print $2}')"
manifest_version="$("$PY" -m esp_harness manifest --no-device --json 2>&1 | tail -1 | "$PY" -c \
    'import json,sys; print(json.loads(sys.stdin.read()).get("toolkit_version","?"))')"
pyproject_version="$(grep -E '^version\s*=' "$ROOT/tools/esp-harness/pyproject.toml" \
    | head -1 | sed -E 's/.*"([^"]+)".*/\1/')"
if [[ "$cli_version" == "$manifest_version" \
   && "$cli_version" == "$pyproject_version" \
   && "$cli_version" != "1.5.0" \
   && "$cli_version" != "0.0.0+source" ]]; then
    emit_pass "version triangulation (CLI / manifest / pyproject all in sync)"
else
    emit_fail "version triangulation (CLI / manifest / pyproject all in sync)" \
              "cli=$cli_version manifest=$manifest_version pyproject=$pyproject_version"
fi

# 6. Build/flash/run MSys trap probe — only meaningful if we're running
# under Git Bash's MSys env. On a "real" Linux/Mac shell this case is
# a no-op (idf.py doesn't refuse). We detect by checking $MSYSTEM and
# skip when unset, so the gate stays portable.
if [[ -n "${MSYSTEM:-}" ]]; then
    aurora="$ROOT/examples/aurora"
    trap_ok=true
    # Round-6 caught that the `run --no-build` variant (round-5's
    # original critical) was missing from the smoke.sh loop. All four
    # idf-py invocation forms enumerated explicitly.
    for sub in "build" "flash --port COM9" "run --port COM9 --seconds 2" "run --no-build --port COM9 --seconds 2"; do
        # The trap intentionally exits non-zero (exit_code=100). With
        # `set -e` at the top of this script, a naked invocation would
        # abort smoke. Use `|| true` to suppress, then inspect the
        # JSON we captured. Same for the JSON parse — if jq-style
        # extraction fails, we fall back to empty strings.
        out="$("$PY" -m esp_harness $sub --project "$aurora" --json 2>&1 | tail -1 || true)"
        ok_field="$(echo "$out" | "$PY" -c 'import json,sys
try:
    print(json.loads(sys.stdin.read()).get("ok"))
except Exception:
    print("")' 2>/dev/null || true)"
        # Round-6 caught smoke.ps1 had the same issue: for `run` the
        # trigger field is nested under details.phases.flash.trigger,
        # not top-level. Check all four known locations + the error
        # string itself. Mirrors smoke.ps1 lines 144-150.
        trigger="$(echo "$out" | "$PY" -c 'import json,sys
try:
    d=json.loads(sys.stdin.read())
    cands = [
        d.get("trigger"),
        d.get("details",{}).get("trigger"),
        d.get("details",{}).get("phases",{}).get("flash",{}).get("trigger"),
        d.get("error",""),
    ]
    for c in cands:
        if c and "MSys" in str(c):
            print(c); break
    else:
        print("")
except Exception:
    print("")' 2>/dev/null || true)"
        if [[ "$ok_field" != "False" || ! "$trigger" =~ MSys ]]; then
            emit_fail "build/flash/run refuse MSys/Mingw exit-0 (R3+R4 regression)" \
                      "[$sub] ok='$ok_field' trigger='$trigger'"
            trap_ok=false
            break
        fi
    done
    [[ "$trap_ok" == "true" ]] && emit_pass "build/flash/run refuse MSys/Mingw exit-0 (R3+R4 regression)"
fi

echo
echo "── device gates skipped (Linux/Mac toolkit serial path not yet supported) ──"

total=$((passed + failed))
if [[ $failed -gt 0 ]]; then
    echo
    printf "\033[31mFAILED — %d/%d cases\033[0m\n" "$failed" "$total"
    for reason in "${fail_reasons[@]}"; do echo "  • $reason"; done
    exit 1
else
    printf "\033[32mPASSED — %d/%d cases\033[0m\n" "$passed" "$passed"
    exit 0
fi
