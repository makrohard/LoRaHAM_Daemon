#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REF_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"

# Only production dispatch/runtime files are checked here.
# client_set.cpp still contains legacy blocking wrappers for tests/compatibility.
files=(
  "$REF_DIR/loradaemon_320_108.cpp"
  "$REF_DIR/radio_channel.cpp"
  "$REF_DIR/config_dispatch.h"
  "$REF_DIR/data_tx.cpp"
)

pattern='\bclient_set_broadcast(_bytes)?[[:space:]]*\('
fail=0

for file in "${files[@]}"; do
  if grep -nE "$pattern" "$file"; then
    fail=1
  fi
done

if [[ "$fail" -ne 0 ]]; then
  echo "[FAIL] blocking broadcast wrapper used in production path" >&2
  exit 1
fi

echo "[ OK ] production paths use queued broadcasts"
