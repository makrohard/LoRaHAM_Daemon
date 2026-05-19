#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REFACTOR_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
BUILD="$REFACTOR_DIR/build.sh"
RUN_TESTS="$REFACTOR_DIR/run_tests.sh"

rc=0

echo "Checking build/test script split..."

require() {
  local file="$1"
  local pattern="$2"
  local label="$3"

  if ! grep -Fq -- "$pattern" "$file"; then
    echo "ERROR: missing build/test split item: $label" >&2
    rc=1
  fi
}

reject() {
  local file="$1"
  local pattern="$2"
  local label="$3"

  if grep -Fq -- "$pattern" "$file"; then
    echo "ERROR: obsolete build/test split item remains: $label" >&2
    rc=1
  fi
}

require "$BUILD" "Build the production daemon binary only." "build.sh production-only help"
require "$BUILD" "build_daemon" "build.sh builds daemon"
require "$BUILD" "build.sh no longer builds tests" "build.sh rejects test mode"
require "$RUN_TESTS" "build_tests" "run_tests owns test build"
require "$RUN_TESTS" '"$SCRIPT_DIR/build.sh"' "run_tests builds daemon first"
require "$RUN_TESTS" "Final test summary" "run_tests remains one-click runner"

reject "$BUILD" "build_tests()" "build.sh must not contain test builder"
reject "$BUILD" "Built test:" "build.sh must not build tests"
reject "$BUILD" "test_data_tx.cpp" "build.sh must not reference test sources"

exit "$rc"
