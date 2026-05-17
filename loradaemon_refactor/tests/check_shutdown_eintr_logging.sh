#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REFACTOR_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
DAEMON="$REFACTOR_DIR/loradaemon_320_108.cpp"

rc=0

echo "Checking shutdown EINTR handling..."

require() {
  local pattern="$1"
  local label="$2"

  if ! grep -Fq -- "$pattern" "$DAEMON"; then
    echo "ERROR: missing shutdown EINTR pattern: $label" >&2
    rc=1
  fi
}

require "#include <errno.h>" "errno include"
require "if (errno == EINTR && daemon_lifecycle_stop_requested())" "EINTR stop guard"
require "daemon_debug_ctx(\"LIFE\", \"Event-Wait durch Stop unterbrochen\")" "debug-only EINTR stop log"
require "perror(\"event_loop_wait\")" "other wait errors stay visible"

exit "$rc"
