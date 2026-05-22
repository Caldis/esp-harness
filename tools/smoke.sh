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

# 2. pytest integration tests.
if pytest_out="$("$PY" -m pytest "$ROOT/tools/esp-harness/tests/" -q 2>&1)"; then
    if echo "$pytest_out" | tail -3 | grep -q "3 passed"; then
        emit_pass "pytest 3/3 integration tests"
    else
        emit_fail "pytest 3/3 integration tests" "$(echo "$pytest_out" | tail -1)"
    fi
else
    emit_fail "pytest 3/3 integration tests" "$(echo "$pytest_out" | tail -1)"
fi

# 3. sim diff regression.
sim_failed="$("$PY" -m esp_harness sim diff \
    --scenes halo,grid,bloom,tilt,pulse,cell,keys,tone,system,glow,spin,notify,track \
    --json 2>&1 | tail -1 | "$PY" -c \
    'import json,sys; d=json.loads(sys.stdin.read()); print(len(d.get("failed",[])))' 2>/dev/null)"
if [[ "$sim_failed" == "0" ]]; then
    emit_pass "sim diff 13 scenes identical"
else
    emit_fail "sim diff 13 scenes identical" "$sim_failed failed"
fi

# 4. manifest exposes >= 17 toolkit cmds.
n_cmds="$("$PY" -m esp_harness manifest --no-device --json 2>&1 | tail -1 | "$PY" -c \
    'import json,sys; d=json.loads(sys.stdin.read()); print(len(d.get("toolkit_commands",[])))' 2>/dev/null)"
if [[ "$n_cmds" -ge 17 ]]; then
    emit_pass "manifest exposes >= 17 toolkit cmds"
else
    emit_fail "manifest exposes >= 17 toolkit cmds" "got $n_cmds"
fi

# 5. version triangulation (regression for v1.7.1 single-source-of-truth fix).
cli_version="$("$PY" -m esp_harness --version 2>&1 | awk '{print $2}')"
manifest_version="$("$PY" -m esp_harness manifest --no-device --json 2>&1 | tail -1 | "$PY" -c \
    'import json,sys; print(json.loads(sys.stdin.read()).get("toolkit_version","?"))')"
if [[ "$cli_version" == "$manifest_version" && "$cli_version" != "1.5.0" ]]; then
    emit_pass "version triangulation matches (--version == manifest)"
else
    emit_fail "version triangulation matches (--version == manifest)" \
              "cli=$cli_version manifest=$manifest_version"
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
