#!/usr/bin/env bash
# CI-style gate: typecheck, unit tests, native HLE checks. Prints a PASS/FAIL table.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
WORKER_ID="${WORKER_ID:-x4-release}"
NATIVE_BUILD="${NATIVE_BUILD:-$ROOT/work/build-native-$WORKER_ID}"
NATIVE_BIN="$NATIVE_BUILD"

declare -a ROWS=()
FAIL=0

run_step() {
  local name="$1"
  shift
  if "$@" > /tmp/check-all-"${name// /_}".log 2>&1; then
    ROWS+=("$name|PASS")
  else
    ROWS+=("$name|FAIL")
    FAIL=1
    tail -20 "/tmp/check-all-${name// /_}.log" >&2 || true
  fi
}


run_step "tsc" npx tsc -p tsconfig.json --noEmit
run_step "vitest" npm test

if [ ! -f "$NATIVE_BUILD/import_coverage" ]; then
  cmake -S "$ROOT/runtime" -B "$NATIVE_BIN" -DDETECTIVE_NO_GAME_CODE=ON > /dev/null
  cmake --build "$NATIVE_BIN" -j"$(nproc)" import_coverage > /dev/null
fi
run_step "import_coverage" "$NATIVE_BUILD/import_coverage" "$ROOT/work/ref/BDETECT.EXE"
run_step "win32_extra_check" bash "$ROOT/runtime/tests/win32_extra_check.sh"

compile_native_test() {
  local name="$1"
  local out="$NATIVE_BUILD/${name}"
  g++ -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter \
    -I"$ROOT/runtime/include" -I"$ROOT/runtime/src" \
    "$ROOT/runtime/tests/${name}.cpp" "$ROOT/runtime/tests/plat_stubs.cpp" \
    "$NATIVE_BUILD/libdetective_runtime.a" -lpthread -lm -o "$out"
  "$out"
}

run_step "files_test" compile_native_test files_test
run_step "ddraw_test" compile_native_test ddraw_test
run_step "dsound_test" compile_native_test dsound_test

printf '\n%-24s %s\n' "Check" "Result"
printf '%.0s-' {1..32}; echo
for row in "${ROWS[@]}"; do
  IFS='|' read -r name result <<<"$row"
  printf '%-24s %s\n' "$name" "$result"
done

exit "$FAIL"
