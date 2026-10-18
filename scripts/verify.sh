#!/usr/bin/env bash
# The whole verification gate in one command, so "I ran the checks" is a
# reproducible statement rather than a claim.
#
#   bash scripts/verify.sh [BUILD_DIR]
#
# This mirrors what CI runs. It deliberately reports SKIP for steps whose tool
# is missing instead of passing them silently: a gate that quietly drops half
# its checks is worse than no gate, because it reads as a green result.
#
# Exit status: 0 when nothing failed, 1 when something failed. A SKIP is not a
# failure, but the summary says so out loud and the caller should read it.

set -uo pipefail
cd "$(dirname "$0")/.."
BUILD="${1:-build}"
PROJECT=charger_project

PASSED=()
FAILED=()
SKIPPED=()

pass() { PASSED+=("$1"); printf '  \033[32mPASS\033[0m  %s\n' "$1"; }
fail() { FAILED+=("$1: $2"); printf '  \033[31mFAIL\033[0m  %s\n' "$1"; printf '        %s\n' "$2" | tail -5; }
skip() { SKIPPED+=("$1: $2"); printf '  \033[33mSKIP\033[0m  %s (%s)\n' "$1" "$2"; }

run() { # run <name> <command...>
    local name="$1"; shift
    local out
    if out="$("$@" 2>&1)"; then
        pass "$name"
    else
        fail "$name" "$(printf '%s' "$out" | tail -5)"
    fi
}

echo "== toolchain =="
command -v cmake >/dev/null || { echo "cmake is required"; exit 1; }
command -v c++   >/dev/null || { echo "a C++ compiler is required"; exit 1; }
cmake --version | head -1
"${CXX:-c++}" --version | head -1
echo

echo "== build and test =="
if out=$(cmake -S "$PROJECT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCHARGER_WERROR=ON 2>&1) &&
   out=$(cmake --build "$BUILD" --parallel 2>&1); then
    pass "build (-Werror, Release)"
else
    fail "build (-Werror, Release)" "$(printf '%s' "$out" | tail -8)"
fi

# Resolve the build directory to an absolute path once. The scripts below chdir
# into charger_project, so a relative path would not reach them; building the
# path by hand ("../$BUILD") silently breaks as soon as BUILD is already absolute.
BUILD_ABS="$(cd "$BUILD" 2>/dev/null && pwd)"
if [ -z "$BUILD_ABS" ]; then
    echo "the build directory $BUILD was not created; stopping"
    exit 1
fi
MON="$BUILD_ABS/charger_monitor"
SETTLE="$BUILD_ABS/charger_settle"
SYNTH="$BUILD_ABS/charger_synth"

run "ctest" ctest --test-dir "$BUILD" --output-on-failure

echo
echo "== contracts =="
run "end-to-end replay"      env CHARGER_MONITOR_BIN="$MON" \
                                 CHARGER_SETTLE_BIN="$SETTLE" \
                                 CHARGER_SYNTH_BIN="$SYNTH" \
                                 bash "$PROJECT/scripts/e2e_replay.sh"
run "consumable package"     env CHARGER_MONITOR_BUILD_DIR="$BUILD_ABS" \
                                 bash "$PROJECT/scripts/consume_test.sh"

# A tampered journal must fail verification, or the audit story is decoration.
tamper() {
    local tmp; tmp="$(mktemp)"
    printf '180#80BBE80300000000\n181#01\n181#02\n181#00\n' |
        "$MON" --replay --journal "$tmp" --replay-interval-ms 1000 >/dev/null 2>&1
    "$MON" --verify-journal --journal "$tmp" >/dev/null || return 1
    awk 'NR==1{sub(/\t/, "\t9\t")} {print}' "$tmp" > "$tmp.bad"
    if "$MON" --verify-journal --journal "$tmp.bad" >/dev/null 2>&1; then
        rm -f "$tmp" "$tmp.bad"; return 1
    fi
    rm -f "$tmp" "$tmp.bad"
}
( cd "$PROJECT" && tamper ) && pass "journal tamper detection" || fail "journal tamper detection" "a tampered journal was accepted"

# The synthetic scenario must be a pure function of its arguments.
determinism() {
    local work; work="$(mktemp -d)"
    "$SYNTH" --out "$work/a.journal" --seed 7 --days 3 >/dev/null 2>&1 &&
    "$SYNTH" --out "$work/b.journal" --seed 7 --days 3 >/dev/null 2>&1 &&
    cmp -s "$work/a.journal" "$work/b.journal" &&
    "$MON" --verify-journal --journal "$work/a.journal" >/dev/null
    local rc=$?
    rm -rf "$work"
    return $rc
}
( cd "$PROJECT" && determinism ) && pass "synthetic journal determinism" || fail "synthetic journal determinism" "two runs of the same seed differ, or the journal does not verify"

echo
echo "== formatting =="
if command -v clang-format >/dev/null; then
    run "clang-format" bash -c "cd '$PROJECT' && clang-format --dry-run --Werror \
        src/*.cpp tests/*.cpp include/*.hpp tools/*/main.cpp examples/consume_library/*.cpp"
else
    skip "clang-format" "clang-format not installed"
fi

echo
echo "== dashboard =="
if command -v python3 >/dev/null; then
    run "committed bundle is current" python3 scripts/build_dashboard.py --check
else
    skip "committed bundle is current" "python3 not installed"
fi

# Reproducibility of the committed goldens is only meaningful on a clean tree:
# otherwise the diff reports whatever else you have in flight.
if [ -n "$(git status --porcelain -- dashboard/data dashboard/tests/golden)" ]; then
    skip "demo data and goldens reproducible" "dashboard/data or tests/golden has uncommitted changes"
else
    if bash dashboard/tests/regen_golden.sh "$BUILD" >/dev/null 2>&1 &&
       git diff --quiet -- dashboard/data dashboard/tests/golden; then
        pass "demo data and goldens reproducible"
    else
        fail "demo data and goldens reproducible" "regenerating changed the committed files; commit the result"
    fi
fi

if command -v node >/dev/null; then
    run "dashboard core tests (C++/JS parity)" env CHARGER_BUILD_DIR="$BUILD_ABS" node --test dashboard/tests/core.test.mjs
else
    skip "dashboard core tests (C++/JS parity)" "node not installed"
fi

echo
echo "============================================================"
printf 'passed %d   failed %d   skipped %d\n' "${#PASSED[@]}" "${#FAILED[@]}" "${#SKIPPED[@]}"
for f in "${FAILED[@]}"; do printf '  FAILED: %s\n' "$f"; done
for s in "${SKIPPED[@]}"; do printf '  SKIPPED: %s\n' "$s"; done
echo "============================================================"

if [ "${#SKIPPED[@]}" -gt 0 ]; then
    echo "The gate was incomplete: read the SKIPPED lines before calling it green."
fi
[ "${#FAILED[@]}" -eq 0 ]
