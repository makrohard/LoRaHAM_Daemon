# LoRaHAM daemon tests

This directory contains the daemon test sources.

## Usage

Run the regular test suite from the daemon source directory:

```bash
./run_tests.sh
```

Or from this `tests/` directory:

```bash
../run_tests.sh
```

The runner builds the production daemon with `build.sh`, builds the test
binaries, and then runs each test against the built daemon binary.

RF transmit tests are disabled by default. Enable them only with suitable
hardware, frequency settings, and RF conditions:

```bash
./run_tests.sh --TX --rx-seconds 15
```

## Runner behavior

`run_tests.sh` currently runs 42 test binaries. It refuses to start if a
`loraham_daemon` process is already running, checks for lingering daemon
processes after each test, parses per-test `Summary:` lines, and prints a final
OK/FAIL/SKIP/XFAIL/XPASS table.

## Test concept

The tests should protect externally relevant behavior:

- daemon build, startup, shutdown, and signal handling
- public command-line behavior, including selected-radio mode
- public UNIX socket behavior and cleanup
- CONFIG command parsing, validation, rejection, and apply safety
- raw DATA socket chunking, framed DATA helpers, and RF packet handling helpers
- client lifecycle, queued output, and slow/nonblocking client behavior
- timing, event-loop, radio-health, and TX-result semantics that affect daemon behavior


## Coverage overview

CONFIG/protocol:

- `test_config_parser`
- `test_config_stream_buffer`
- `test_config_stream`
- `test_config_value`
- `test_config_policy`
- `test_config_validate`
- `test_config_apply_transactional`
- `test_config_dispatch`

DATA/RF/TX:

- `test_data_tx`
- `test_data_tx_queue_runtime`
- `test_tx_result`
- `test_daemon_tx_outcome`
- `test_daemon_tx_job`
- `test_daemon_tx_executor`
- `test_daemon_tx_completion`
- `test_daemon_tx_queue`
- `test_daemon_tx_worker`
- `test_daemon_tx_async_worker`
- `test_daemon_tx_async_runtime`
- `test_radio_controller_tx_worker`
- `test_rf_packet`
- `test_framed_data` (including `TX_RESULT` layout)
- `test_framed_data_tx`
- `test_tx_failure_keeps_client`

Client/socket/runtime:

- `test_client_output_queue`
- `test_client_nonblocking`
- `test_client_queued_broadcast`
- `test_client_slow_output`
- `test_event_loop_output_flush`
- `test_client_lifecycle`
- `test_rssi_multiclient`
- `test_unix_socket`

Lifecycle/helper behavior:

- `test_daemon_radio_selection`
- `test_event_loop`
- `test_daemon_timing`
- `test_daemon_lifecycle`
- `test_radio_health`
- `test_radio_cad_probe`

Public integration baseline:

- `test_interface_baseline`


## CAD/TX rework guardrail

`test_tx_failure_keeps_client` verifies the M1 behavior: recoverable RF/TX execution failures are reported through the existing ERROR path without closing the framed client connection.

- `test_conf_status` covers `TXRESULT`, `TXMODE`, and `GET CHANNEL` CONF state reporting.
- `test_radio_cad_probe` verifies the real-CAD helper and the M3c RAW-mode one-probe TX guard behavior.

- `test_daemon_tx_outcome` verifies internal TX outcome to framed TX_RESULT status mapping.

- `test_daemon_tx_job` verifies future TX job/result data structures without changing TX behavior.

- `test_daemon_tx_executor` verifies the synchronous TX executor seam and raw `TxResult` preservation without changing daemon TX behavior.

- `test_daemon_tx_queue` verifies the bounded TX queue contract and synchronous drain seam without changing daemon TX behavior.

- `test_daemon_tx_worker` verifies the future TX worker state facade without changing daemon TX behavior.

- `test_data_tx_queue_runtime` verifies the opt-in DATA TX async queue path, last-completion bookkeeping, target propagation, and completion queue handoff while keeping default DATA TX direct.

- `test_radio_controller_tx_worker` verifies that each radio controller owns initialized TX worker state.

- `test_daemon_tx_async_worker` verifies the standalone async TX worker skeleton without wiring it into daemon runtime.

- `test_daemon_tx_async_runtime` verifies daemon-owned async TX worker lifecycle state without live TX routing.

- `test_daemon_tx_completion` verifies encoding internal TX completion results as framed `TX_RESULT` frames, bounded completion queue behavior, targeted slot delivery, and main-loop drain delivery.
