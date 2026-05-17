#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REFACTOR_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
DAEMON="$REFACTOR_DIR/loradaemon_320_108.cpp"
COMMON="$REFACTOR_DIR/tests/common_loradaemon_test.h"
IFACE="$REFACTOR_DIR/tests/test_interface_baseline.c"

rc=0

echo "Checking debug/verbose CLI logging foundation..."

require() {
  local file="$1"
  local pattern="$2"
  local label="$3"

  if ! grep -Fq -- "$pattern" "$file"; then
    echo "ERROR: missing debug CLI/logging pattern: $label" >&2
    rc=1
  fi
}

require "$DAEMON" "DAEMON_LOG_VERBOSE" "verbose log level"
require "$DAEMON" "DAEMON_LOG_DEBUG" "debug log level"
require "$DAEMON" "static DaemonLogLevel daemon_log_level" "global log level"
require "$DAEMON" "static void daemon_verbose(const char *fmt, ...)" "verbose helper"
require "$DAEMON" "static void daemon_debug(const char *fmt, ...)" "debug helper"
require "$DAEMON" "static void daemon_debug_band(const char *tag, const char *fmt, ...)" "band debug helper"
require "$DAEMON" "{\"verbose\", no_argument, 0, 'v'}" "long verbose option"
require "$DAEMON" "{\"debug\",   no_argument, 0, 1000}" "long debug option"
require "$DAEMON" "getopt_long(argc, argv, \"dvh\", long_options, NULL)" "getopt_long parser"
require "$DAEMON" "[-d] [-v|--verbose] [--debug] [--help]" "usage text"
require "$COMMON" "test_cli_help_option" "CLI help option test helper"
require "$IFACE" "CLI accepts -v" "short verbose interface test"
require "$IFACE" "CLI accepts --verbose" "long verbose interface test"
require "$IFACE" "CLI accepts --debug" "debug interface test"

exit "$rc"
