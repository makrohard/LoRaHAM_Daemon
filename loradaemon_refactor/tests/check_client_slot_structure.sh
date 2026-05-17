#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REFACTOR_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
HEADER="$REFACTOR_DIR/client_slot.h"
SOURCE="$REFACTOR_DIR/client_slot.cpp"
DISPATCH="$REFACTOR_DIR/config_dispatch.h"
CHANNEL_H="$REFACTOR_DIR/radio_channel.h"
CHANNEL_CPP="$REFACTOR_DIR/radio_channel.cpp"
DAEMON="$REFACTOR_DIR/loradaemon_320_108.cpp"
DATA_TX_H="$REFACTOR_DIR/data_tx.h"
DATA_TX_CPP="$REFACTOR_DIR/data_tx.cpp"

rc=0

echo "Checking ClientSlot structure..."

require() {
  local file="$1"
  local pattern="$2"
  local label="$3"

  if ! grep -Fq -- "$pattern" "$file"; then
    echo "ERROR: missing ClientSlot structure item: $label" >&2
    rc=1
  fi
}

reject() {
  local file="$1"
  local pattern="$2"
  local label="$3"

  if grep -Fq -- "$pattern" "$file"; then
    echo "ERROR: obsolete ClientSlot compatibility item remains: $label" >&2
    rc=1
  fi
}

require "$HEADER" "typedef struct {" "ClientSlot typedef"
require "$HEADER" "int fd;" "fd stored in ClientSlot"
require "$HEADER" "ClientOutputQueue output;" "output queue stored in ClientSlot"
require "$HEADER" "ConfigStreamBuffer stream;" "CONFIG stream stored in ClientSlot"
require "$SOURCE" "client_output_queue_reset(&slot->output)" "output reset with slot"
require "$SOURCE" "config_stream_init(&slot->stream)" "stream reset with slot"
require "$DISPATCH" "ClientSlot *slots" "CONFIG dispatch uses ClientSlot"
require "$DISPATCH" "slot->stream" "CONFIG dispatch uses slot stream"
require "$DISPATCH" "client_slot_close(slot)" "CONFIG dispatch closes whole slot"
require "$DATA_TX_H" "data_tx_process_slots" "DATA TX has ClientSlot API"
require "$DATA_TX_CPP" "client_slot_ready(slot, readfds)" "DATA TX reads ClientSlot readiness"
require "$DATA_TX_CPP" "client_slot_close(slot)" "DATA TX closes whole slot"
require "$CHANNEL_H" "ClientSlot *data_slots;" "RadioChannelIo stores DATA ClientSlot"
require "$CHANNEL_H" "ClientSlot *conf_slots;" "RadioChannelIo stores CONF ClientSlot"
require "$CHANNEL_CPP" "client_slot_accept_with_output(*ch->data_listen_fd" "DATA accept uses ClientSlot"
require "$CHANNEL_CPP" "client_slot_accept_with_output(*ch->conf_listen_fd" "CONF accept uses ClientSlot"
require "$CHANNEL_CPP" "client_slot_flush_ready_outputs(ch->data_slots" "DATA flush uses ClientSlot"
require "$CHANNEL_CPP" "client_slot_flush_ready_outputs(ch->conf_slots" "CONF flush uses ClientSlot"
require "$DAEMON" "ClientSlot client_data433_slots" "daemon has 433 DATA ClientSlot table"
require "$DAEMON" "ClientSlot client_data868_slots" "daemon has 868 DATA ClientSlot table"
require "$DAEMON" "ClientSlot client_conf433_slots" "daemon has 433 CONF ClientSlot table"
require "$DAEMON" "ClientSlot client_conf868_slots" "daemon has 868 CONF ClientSlot table"
require "$DAEMON" "client_slot_init_all(client_data433_slots, MAX_CLIENTS)" "433 DATA ClientSlot init"
require "$DAEMON" "client_slot_init_all(client_data868_slots, MAX_CLIENTS)" "868 DATA ClientSlot init"
require "$DAEMON" "data_tx_process_slots(\"433\"" "433 DATA TX uses ClientSlot"
require "$DAEMON" "data_tx_process_slots(\"868\"" "868 DATA TX uses ClientSlot"
require "$DAEMON" "client_slot_broadcast_bytes_queued(client_data433_slots" "433 DATA RX broadcast uses ClientSlot"
require "$DAEMON" "client_slot_broadcast_bytes_queued(client_data868_slots" "868 DATA RX broadcast uses ClientSlot"
require "$DAEMON" "client_slot_broadcast_queued(client_conf433_slots" "433 CONF broadcasts use ClientSlot"
require "$DAEMON" "client_slot_broadcast_queued(client_conf868_slots" "868 CONF broadcasts use ClientSlot"
require "$REFACTOR_DIR/build.sh" '"$SCRIPT_DIR/client_slot.cpp"' "client_slot linked in build"
require "$REFACTOR_DIR/run_tests.sh" '"$TEST_DIR/test_client_slot"' "test_client_slot in run_tests"

reject "$DATA_TX_H" "data_tx_process_clients(" "old DATA TX clients API"
reject "$DATA_TX_H" "data_tx_process_clients_with_output" "old DATA TX clients-with-output API"
reject "$DATA_TX_CPP" "void data_tx_process_clients(" "old DATA TX clients implementation"
reject "$DATA_TX_CPP" "void data_tx_process_clients_with_output" "old DATA TX clients-with-output implementation"
reject "$REFACTOR_DIR/run_tests.sh" "test_client_read_disconnect_cleanup" "old read-disconnect cleanup test"

for pattern in \
  'client_data433' \
  'client_data868' \
  'output_data433' \
  'output_data868' \
  'client_conf433' \
  'client_conf868' \
  'output_conf433' \
  'output_conf868' \
  'config_stream_conf433' \
  'config_stream_conf868'
do
  if grep -RInE "(^|[^A-Za-z0-9_])${pattern}([^A-Za-z0-9_]|$)" "$REFACTOR_DIR" \
      --include='*.cpp' --include='*.h'; then
    echo "ERROR: old client-array symbol remains: $pattern" >&2
    rc=1
  fi
done

exit "$rc"
