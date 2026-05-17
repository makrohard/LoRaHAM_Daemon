#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REFACTOR_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
DAEMON="$REFACTOR_DIR/loradaemon_320_108.cpp"
HEADER="$REFACTOR_DIR/radio_controller.h"
BUILD="$REFACTOR_DIR/build.sh"

rc=0

echo "Checking RadioController unique_ptr ownership..."

require() {
  local file="$1"
  local pattern="$2"
  local label="$3"

  if ! grep -Fq -- "$pattern" "$file"; then
    echo "ERROR: missing unique_ptr ownership pattern: $label" >&2
    rc=1
  fi
}

forbid() {
  local file="$1"
  local pattern="$2"
  local label="$3"

  if grep -Fq -- "$pattern" "$file"; then
    echo "ERROR: obsolete raw ownership pattern remains: $label" >&2
    rc=1
  fi
}

require "$HEADER" "#include <memory>" "memory include"
require "$HEADER" "std::unique_ptr<PiHal> hal" "HAL unique_ptr field"
require "$HEADER" "std::unique_ptr<Module> mod" "Module unique_ptr field"
require "$HEADER" "std::unique_ptr<RadioT> radio" "radio unique_ptr field"
require "$DAEMON" "radio_controller_433.hal.reset(new PiHal(0));" "433 HAL unique_ptr allocation"
require "$DAEMON" "radio_controller_433.mod.reset(new Module(radio_controller_433.hal.get()" "433 Module uses HAL raw view"
require "$DAEMON" "radio_controller_433.radio.reset(new SX1278(radio_controller_433.mod.get()))" "433 radio unique_ptr allocation"
require "$DAEMON" "radio_controller_868.hal.reset(new PiHal(0));" "868 HAL unique_ptr allocation"
require "$DAEMON" "radio_controller_868.mod.reset(new Module(radio_controller_868.hal.get()" "868 Module uses HAL raw view"
require "$DAEMON" "radio_controller_868.radio.reset(new RFM95(radio_controller_868.mod.get()))" "868 radio unique_ptr allocation"
require "$DAEMON" "ctrl->radio.reset();" "shutdown releases radio"
require "$DAEMON" "ctrl->mod.reset();" "shutdown releases module"
require "$DAEMON" "ctrl->hal.reset();" "shutdown releases HAL"
require "$DAEMON" "radio_channel_read_live_rssi(ctrl->mod.get()" "RSSI passes raw Module view"
require "$BUILD" "RadioLib not found for radio controller skeleton test" "skeleton test has RadioLib includes"
require "$BUILD" "RadioLib not found for radio controller accessors test" "accessor test has RadioLib includes"

forbid "$HEADER" "PiHal *hal" "raw HAL field"
forbid "$HEADER" "Module *mod" "raw Module field"
forbid "$HEADER" "RadioT *radio" "raw radio field"
forbid "$DAEMON" "delete ctrl->radio" "raw radio delete"
forbid "$DAEMON" "delete ctrl->mod" "raw module delete"
forbid "$DAEMON" "delete ctrl->hal" "raw HAL delete"
forbid "$DAEMON" "radio_controller_433.radio = new" "433 raw radio assignment"
forbid "$DAEMON" "radio_controller_868.radio = new" "868 raw radio assignment"

exit "$rc"
