# Changelog

## loraham_daemon 110

- Standalone async TX worker skeleton added for future queued runtime TX.
- TXQUEUE opt-in DATA TX path now uses bounded queue with synchronous drain.
- TXQUEUE opt-in configuration and status visibility added without changing TX path.
- Per-radio TX worker state and passive GET STATUS queue counters added.
- TX worker state facade added around the bounded queue for future threaded TX.
- Bounded TX queue contract and synchronous drain seam added for future worker-thread TX.
- Synchronous DATA TX path now routes through the TX executor seam.
- Synchronous TX executor seam added for future worker-thread integration.
- TX job/result structs added as preparation for future async TX worker.
- Fix TX outcome test string literals so the M5a test builds.
- Internal TX outcomes now map separately to framed TX_RESULT wire statuses.
- GET CHANNEL reports per-band radio, CAD, RSSI, mode, and TX mode snapshots.
- TX guard now uses the real CAD probe and reports CAD-blocked framed TX as CHANNEL_BUSY.
- Real CAD probe helper added for later TXMODE and GET CHANNEL wiring.
- CONF TXMODE state can be set per band and is reported in GET STATUS.
- Framed DATA TX_RESULT can be enabled per band and emitted after TX_PACKET attempts.
- Framed DATA protocol defines TX_RESULT frame layout and encoder.
- Framed DATA TX execution failures now emit ERROR and keep the parser/client alive.
- CAD/TX rework guardrail: add expected-failing framed TX failure characterization.

- Framed DATA `RX_PACKET` now prepends RSSI/SNR metadata before RF bytes.

## loraham_daemon 109a

- Refactor / Hardening
  - Modularized loraham_daemon main module
  - General code cleanup.

- Bugfixes
  - Record CAD timeouts through the TX result stats path.
  - Check daemon background-mode directory and stdio redirection failures.
  - Acquire TX busy state atomically before copying/logging payloads.
  - fd `0` client handling with explicit `-1` client-set initialization.

- New Features
  - CONF status: broadcast local TX state as `TX=1` / `TX=0`.
  - CONF status: add `GET STATUS` runtime snapshot reply.
  - Runtime stats: add hourly operator stats and `GET STATS` on CONF sockets.

## loraham_daemon 109

Refactoring by Johannes Loose
Initial version: loradaemon_320_108 by Alexander Walter

- Refactor / Hardening
  - Event loop: moved polling/socket loop toward event-backend structure with test coverage.
  - Socket handling: hardened client slots, nonblocking I/O, queued broadcasts, and slow-client behavior.
  - RadioController/OOP: replaced radio globals and centralized 433/868 runtime state in `RadioController`.
  - Radio lifecycle: moved RadioLib objects to `std::unique_ptr` ownership with explicit shutdown.
  - TX path: routed DATA-TX, CAD guard, band/mode/health, and send flow through `RadioController`.
  - RX path: structured RX packet flow, IRQ/FIFO handling, and broadcast path through `RadioController`.
  - CAD/RSSI: routed polling and RSSI streaming through controller state instead of legacy mirrors.
  - CONFIG path: CONFIG dispatch now uses `RadioController` as runtime/hardware source.
  - TX logging: show compact TX packet preview in normal output.
  - TX logging: print full normal-mode ASCII packet preview.
  - LED path: radio-flow LED handling now goes through `RadioController`.
  - Logging module: extracted logger implementation into `daemon_log.h/.cpp`.
  - Test hardening: expanded structural guards and integration/regression coverage in `run_tests.sh`.
  - Harden RX packet handling: validate RX lengths, drop invalid packets safely, and avoid parsing short LoRa payloads as metadata.
  - Harden CONFIG command validation: require exact SET commands and reject unknown CONFIG keys.
  - Harden CONFIG apply: abort remaining changes after failed MODE switch and defer GETRSSI until mode switching succeeds.

- Bugfixes
  - Fix CONFIG stream framing: fragmented commands are buffered and newline-separated commands are processed individually.
  - Fix client broadcast errors: failed writes close broken clients instead of keeping stale slots.
  - Fix client slot overflow: accepted clients without a free slot are closed instead of leaked.
  - Fix FSK SHAPING parsing: BT values now map to RadioLib constants instead of truncating to 0.
  - Fix TX bounds checks: invalid or oversized packets are rejected before copy/transmit.
  - Fix RX error forwarding: RadioLib CRC/header/read errors are dropped and counted.
  
- New Features  
  - Build Script `./build.sh`  
  - Testsuite `./run_tests.sh`  
  - Debug logging`--debug`  
  - Added `--help`  
  - Version now lives in `daemon_version.h`
  - Added one-radio-mode --radio 433 | 868 | both
  - Add framed DATA sockets with packet-boundary-preserving TX/RX frames, shared-radio behavior 
