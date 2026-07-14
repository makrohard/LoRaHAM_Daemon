# Changelog

## loraham_daemon 112
- Multi-hardware M4: `Sx1262Driver` activates `--hw waveshare-sx1262` (LF and HF binding): TCXO via DIO3 in begin()/beginFSK(), DIO2-as-RF-switch + TXEN, SX126x CRC/sync/power/LDRO semantics, no-OOK rejected, live RSSI via GetRssiInst; without the HAT begin() fails closed with one diagnosis line (suite-tested on the bench).
- Multi-hardware M3: chip access extracted into a runtime `RadioDriver` interface (`radio_driver.h`, `sx127x_driver.{h,cpp}`); `RadioController` and all runtime paths are chip-agnostic (no SX1278/RFM95 names or SX127x register constants outside the driver). Behavior-identical: full suite and live legacy-hardware smoke (STATUS/CHANNEL/TX) unchanged.
- Multi-hardware M2: `--hw <preset>` hardware profiles (`legacy` default = pre-profile wiring bit-identical; `uputronics-ce0/ce1` with NC DIO1/RESET, capability-gated CAD (passive-RSSI fallback for MANAGED TX and `GET CHANNEL`, `CADSCAN=0`), slot-based LEDs, warm-start note; `waveshare-sx1262` preset resolves, init fails closed until the SX1262 driver milestone). Profile-aware `begin()` failure diagnosis (RegVersion heuristic), LED-off profiles (`led_pin` NC) run cleanly without claims, boot TX-mode/CAD apply and logs restricted to the selected band (P3-1).
- Version bump to 112; full-suite verification against the 111a baseline (no single-band regression).
- Docs/comments: removed all in-process dual-radio descriptions; README documents mandatory `--radio`, single-band socket exposure, and a migration note (`--radio both` → `loraham-daemon@433` + `@868`; banded flags → plain flags in per-unit overrides).
- BREAKING: removed the band-suffixed CLI overrides `--tx-mode-433/868`, `--cad-monitor-433/868`, `--cad-rssi-433/868` (rejected as unknown options). The plain `--tx-mode`, `--cad-monitor`, `--cad-rssi` are equivalent in a single-band process; per-band differentiation moves to per-unit systemd overrides.
- Instance lock and LED claim reduced to single-band acquisition (no dual-lock ordering/rollback); exit codes 3/4 and LOCK_NB semantics unchanged.
- BREAKING: `--radio both` and the implicit "both" default removed; `--radio 433|868` is mandatory (missing/invalid selection fails closed via the usage-error path). One radio per process; dual-band runs `loraham-daemon@433` + `loraham-daemon@868`.

## loraham_daemon 111a
- loraham_daemon can now safely run multiple instances
- CAD monitor: fixed lost `CAD=0` when RX was pending; free now confirms after 2 samples 3 dB below `CADRSSI`

## loraham_daemon 111

- CAD/TX was reworked: MANAGED TX now performs bounded CAD/LBT with a stable
  idle window and returns `CHANNEL_BUSY` on timeout (default 1.5 s) instead of
  transmitting, while DIRECT TX transmits immediately with no CAD gating
  (selectable at boot per band via `--tx-mode`/`--tx-mode-433`/`--tx-mode-868`
  or at runtime via `SET TXMODE`); legacy/raw clients need `DIRECT` for
  backward compatibility. Per-band CAD policy
  (CADWAIT/CADIDLE/CADPOLL/CADTXAFTERTIMEOUT) is now configurable via CONF and
  reported in `GET STATUS`.
- DATA TX now runs through a bounded async queue, enabled by default
  (`SET TXQUEUE=0` keeps the direct path). Daemon-owned worker threads own the
  CAD/LBT decision, queued jobs retain their CAD callback, and completions are
  delivered in the main loop as final `TX_RESULT` frames with preserved sequence
  numbers and client-slot generation checks.
- RadioLib access now uses a per-radio guard across TX, RX, CAD, RSSI,
  monitoring, and CONFIG apply. CAD probes restore LoRa RX after scanning, and
  CAD monitoring uses a dedicated broadcast latch, scanning at most every 200 ms
  and only for subscribed CONF clients.
- Status and framed protocol were extended: `GET CHANNEL` reports per-band
  radio/CAD/RSSI/mode/TXMODE snapshots (LIVERSSI, CADSCAN, CADSTATE, PACKETRSSI)
  and returns immediately during active TX; framed `TX_RESULT` is configurable
  per band; STATUS exposes queue and completion counters including
  `TXQRESULTDROP`.
- Shutdown now discards queued TX jobs while letting only the in-flight job
  finish and rejects jobs submitted after shutdown begins. Async completion
  statistics use an atomic worker-to-runtime handoff and stay in sync with
  `GET STATS`/`GET STATUS`.
- Build, test, and operational hardening: release builds are hardened and CI
  runs pinned-RadioLib normal/strict/ASan-UBSan/TSan suites with TX-worker
  stress coverage; sockets use 0660 with a restrictive daemon umask; epoll
  supports 128 FDs and stops cleanly on registration errors; the per-band
  activity LED is driven by a single derived-state writer (transmit or
  CAD/channel busy) so it can no longer latch on, with partial-claim cleanup.
  README documents the current TX/CAD/queue/completion/CONF interfaces.
- Replaced per-loop epoll reset/re-registration with persistent watch
  reconciliation, dynamic EPOLLOUT interest, stale-watch cleanup, FD-reuse
  protection, and regression coverage.


## loraham_daemon 110

- Framed DATA `RX_PACKET` now prepends RSSI/SNR metadata before RF bytes.
*Breaks data socket interface dompatibility with 109a*

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
