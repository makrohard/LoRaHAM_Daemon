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

`run_tests.sh` currently runs 56 test binaries. It refuses to start if a
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
- `test_daemon_tx_policy`
- `test_daemon_tx_job`
- `test_daemon_tx_executor`
- `test_daemon_tx_completion`
- `test_daemon_tx_queue`
- `test_daemon_tx_worker`
- `test_daemon_tx_async_worker`
- `test_daemon_tx_async_runtime`
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
- `test_hardware_profile` (`--hw` preset resolution: legacy per-band identity, Uputronics NC pins/capabilities/slot LEDs, Waveshare SX1262 pin set, unknown-preset rejection)
- `test_tx_mode_boot` (`--tx-mode` parsing, default MANAGED, fail-closed on invalid values)
- `test_cad_monitor_boot` (`--cad-monitor` parsing, default off, fail-closed on invalid values)
- `test_event_loop` (persistent reconciliation, mask changes, stale removal, and fd reuse)
- `test_daemon_timing`
- `test_daemon_lifecycle`
- `test_radio_health`
- `test_radio_cad_probe` (incl. capability gating: profiles without DIO1 never call scanChannel and answer from the passive RSSI probe)
- `test_cad_monitor_state` (opt-in `CAD=0/1` CONF monitor: single-edge emission, RX-pending must not suppress `CAD=0`, free-confirmation hysteresis/dead band, non-destructive to RX, and latch-reset semantics)

Multi-instance (split per-band) operation:

- `test_daemon_led` (selection-aware LED ownership: 433-only / 868-only claims, duplicate-band rejection is fatal, and profile-disabled LED (`led_pin` NC) stays healthy without claims)
- `test_instance_lock` (per-band instance-ownership locks: 433/868 ownership, duplicate rejection, release-unblocks-restart, and shared-lock inode stability)
- `test_locking_pihal` (process-shared SPI transaction lock: cross-process exclusion, recursion guard, fail-closed when the lock dir is unusable, no transfer without the lock, EINTR-retry vs hard-failure on both lock and unlock, and fatal-on-hard-unlock; no radio hardware needed)
- `test_runtime_lockdir` (trusted lock-directory/file validation: missing, symlink, non-directory, group/world-writable, non-root-owner-when-required, regular-file and non-regular/symlink lock files, and override-mode directory creation)
- `test_packaging` (deployment artifacts: `systemd/tmpfiles.d/loraham.conf` exists and documents `/run/lock/loraham`; the unit has no `RuntimeDirectory`/`EnvironmentFile` and keeps `RestartPreventExitStatus`)
- `test_multi_instance` (integration: duplicate same-band rejection with socket survival, simultaneous 433+868, and independent shutdown; requires radio hardware and reports `SKIP` without it)

Public integration baseline:

- `test_interface_baseline` (CLI incl. `--hw` preset acceptance/rejection, per-band socket exposure, waveshare-profile fail-closed without HAT, LoRa/FSK config, RF write paths)


## CAD/TX rework guardrail

`test_tx_failure_keeps_client` verifies the M1 behavior: recoverable RF/TX execution failures are reported through the existing ERROR path without closing the framed client connection.

- `test_conf_status` covers `TXRESULT`, `TXMODE`, and `GET CHANNEL` CONF state reporting.
- `test_radio_cad_probe` verifies the real-CAD helper and the DIRECT-mode immediate (no-CAD) TX behavior.

- `test_daemon_tx_outcome` verifies internal TX outcome to framed TX_RESULT status mapping.

- `test_daemon_tx_job` verifies future TX job/result data structures without changing TX behavior.

- `test_daemon_tx_executor` verifies the synchronous TX executor seam and raw `TxResult` preservation without changing daemon TX behavior.

- `test_daemon_tx_queue` verifies the bounded TX queue contract and synchronous drain seam without changing daemon TX behavior.

- `test_daemon_tx_worker` verifies the synchronous TX worker test facade and drain seam.

- `test_data_tx_queue_runtime` verifies the opt-in DATA TX async queue path, last-completion bookkeeping, target/sequence/generation propagation, completion queue handoff, RAW/MANAGED CAD wait policy behavior, MANAGED stable-idle enforcement, CAD-timeout flag preservation, and synchronous TX-busy timeout behavior with fast bounded test limits while keeping default DATA TX direct.

- `test_daemon_tx_async_worker` verifies the standalone async TX worker lifecycle.

- `test_daemon_tx_async_worker_stress` verifies concurrent submit/stop accounting.

- `test_daemon_tx_async_runtime` verifies daemon-owned async TX worker lifecycle state without live TX routing.

- `test_daemon_tx_completion` verifies encoding internal TX completion results as framed `TX_RESULT` frames, bounded completion queue behavior, targeted slot delivery, stale slot-generation rejection and accounting, main-loop drain delivery, and final-only queued result policy.

- `test_daemon_tx_policy` verifies the central CAD/TX timing policy constants and pure helper behavior used by the DATA TX CAD wait path.

- `test_daemon_stats_cad_timeout_send` verifies the dedicated MANAGED send-after-CAD-timeout statistics field.
