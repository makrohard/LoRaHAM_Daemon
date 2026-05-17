#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
README="$SCRIPT_DIR/../README.md"

rc=0

echo "Checking README CONFIG policy documentation..."

require() {
  local pattern="$1"
  local label="$2"

  if ! grep -Fq -- "$pattern" "$README"; then
    echo "ERROR: README missing CONFIG policy: $label" >&2
    rc=1
  fi
}

require "Values are parsed strictly" "strict value parsing note"
require 'integer `7` to `12`' "LoRa SF range"
require 'exact `7.8`, `10.4`, `15.6`, `20.8`, `31.25`, `41.7`, `62.5`, `125`, `250`, `500` kHz' "LoRa bandwidth list"
require 'integer `5` to `8`' "LoRa CR range"
require 'LoRa: `0x00` to `0xFF` or decimal `0` to `255`; FSK: 1 or 2 non-zero bytes, max `0xFFFF`' "SYNC policy"
require 'strict number `0.5` to `300.0` kbps' "FSK bitrate range"
require 'strict number `>0` to `200.0` kHz; RadioLib may reject invalid BR/FREQDEV combinations' "FSK frequency deviation range"
require 'exact `2.6`, `3.1`, `3.9`, `5.2`, `6.3`, `7.8`, `10.4`, `12.5`, `15.6`, `20.8`, `25.0`, `31.25`, `31.3`, `41.7`, `50.0`, `62.5`, `83.3`, `100.0`, `125.0`, `166.7`, `200.0`, `250.0` kHz' "FSK RXBW list"
require 'exact `off`, `none`, `0.0`, `0.3`, `0.5`, `0.7`, `1.0`' "FSK shaping values"

exit "$rc"
