#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REFACTOR_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
DAEMON="$REFACTOR_DIR/loradaemon_320_108.cpp"

rc=0

echo "Checking startup/lifecycle debug logging..."

require() {
  local pattern="$1"
  local label="$2"

  if ! grep -Fq -- "$pattern" "$DAEMON"; then
    echo "ERROR: missing startup/lifecycle log pattern: $label" >&2
    rc=1
  fi
}

require "daemon_debug_ctx(\"STARTUP\", \"SIGPIPE wird ignoriert\")" "SIGPIPE debug log"
require "daemon_verbose_ctx(\"STARTUP\", \"Daemon-Modus aktiv\")" "daemon mode verbose log"
require "daemon_verbose_ctx(\"STARTUP\", \"Verbose aktiv\")" "verbose CLI log"
require "daemon_debug_ctx(\"STARTUP\", \"Debug aktiv\")" "debug CLI log"
require "daemon_verbose_ctx(\"STARTUP\", \"Startmodus: %s\"" "startup mode verbose log"
require "daemon_debug_ctx(\"STARTUP\", \"Argumente verarbeitet\")" "argument debug log"
require "daemon_debug_ctx(\"STARTUP\", \"Starte Radio- und Socket-Init\")" "startup init debug log"
require "daemon_debug_ctx(\"STARTUP\", \"Startup abgeschlossen\")" "startup done debug log"
require "daemon_debug_ctx(\"LIFE\", \"Initialisiere Laufzeitkontext\")" "runtime context init debug log"
require "daemon_verbose_ctx(\"LIFE\", \"Polling aktiv\")" "polling verbose log"
require "daemon_log(\"Stop angefordert\")" "German normal stop log"
require "daemon_verbose_ctx(\"LIFE\", \"Shutdown beginnt\")" "shutdown verbose log"
require "daemon_debug_ctx(\"LIFE\", \"Stoppe Funkmodule\")" "radio shutdown debug log"
require "daemon_debug_ctx(\"LIFE\", \"Schließe Event-Backend\")" "event backend shutdown debug log"
require "daemon_debug_ctx(\"LIFE\", \"Schließe Clients\")" "client shutdown debug log"
require "daemon_debug_ctx(\"LIFE\", \"Entferne Socket-Dateien\")" "socket cleanup debug log"
require "daemon_debug_ctx(\"LIFE\", \"Shutdown abgeschlossen\")" "shutdown done debug log"

exit "$rc"
