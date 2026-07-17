# Changelog

## loraham_daemon 112

- Companion clients (chat, iGate 105d/106, rssi_dualbar, mt_decrypt parser) resolve the daemon sockets automatically: `/run/loraham` when the daemon serves there (systemd), `/tmp` fallback otherwise — one client build works in both deployments; direct daemon runs use `LORAHAM_SOCKET_DIR=/tmp`.
- Hardware lock files (instance/SPI/GPIO) are owner-only 0600 (legacy 0660 corrected via fchmod on open) — a loraham socket-client user can no longer hold hardware locks, block startup, or force runtime SPI timeouts. The daemon itself runs unprivileged as the dedicated `loraham` system user (hardware via the spi/gpio device groups, never root); the trusted lock-dir check accepts root or the daemon user as owner.
- Startup lock-infrastructure failures (held GPIO pin lock, unusable spi0.lock) now exit exactly 4 (restart-suppressed) after normal cleanup; genuine LED/radio hardware failures keep the restartable exit 1. Exact-exit-code integration tests added.
- Invalid reserved runtime setters (`SET CADWAIT=1`, `TXQUEUE=2`, `TXMODE=foo`, …) answer `ERR INVALID` (or `ERR MALFORMED` when incomplete) independent of radio readiness, instead of falling through to `ERR UNKNOWN`/`ERR RADIO_NOT_READY`.
- BEHAVIOR: stable per-command CONF replies — every complete CONF command line gets exactly one response (`OK` / `ERR MALFORMED|UNKNOWN|INVALID|BUSY|RADIO_NOT_READY|HARDWARE`); `GET STATUS/STATS/CHANNEL` keep their single data line with no trailing `OK`; broadcasts unchanged.
- BEHAVIOR: TXQUEUE drain-before-disable — `SET TXQUEUE=0` is rejected (`ERR BUSY`) while async jobs are pending or executing; the direct DATA path additionally refuses residual async work as BUSY (a direct CAD+TX can never stack behind queued CAD+TX).
- BEHAVIOR: public sockets moved from `/tmp` to the protected `/run/loraham` (loraham:loraham 2750 setgid via tmpfiles.d, `UMask=0007` in the unit; the daemon runs as the unprivileged `loraham` user and owns the directory) — no compatibility paths in `/tmp`; `LORAHAM_SOCKET_DIR` overrides for dev/test only. Lock namespace `/run/lock/loraham` unchanged.
- BEHAVIOR: GPIO pin locks are acquired before the FIRST GPIO access (LED claim included) via a testable claim-then-init seam; every startup-failure path and shutdown releases them (locks before instance lock, hardware torn down first).
- Runtime SPI fatals exit with the new code 5 (`LORAHAM_EXIT_RUNTIME_SPI_ERROR`, restart-eligible); exit 4 stays reserved for non-restartable startup lock-infrastructure failures (`RestartPreventExitStatus=3 4`).
- SX1262 rejects EVERY `OOK` key (including `OOK=0`) at prevalidation with `ERR INVALID`; the driver branch returns a non-success state as defense in depth.
- New Hardware Support:
  - Uputronics Raspberry PiZero LoRa(TM) Expansion Board V2.5C
  - Waveshare SX1262 LoRaWAN/GNSS HAT

- Removed double stack to simplify code. That implies breaking the API:

- BREAKING: `--radio both` and the implicit "both" default removed; `--radio 433|868` is mandatory (missing/invalid selection fails closed via the usage-error path). One radio per process; dual-band runs `loraham-daemon@433` + `loraham-daemon@868`.

- BREAKING: the band-suffixed CLI flags `--tx-mode-433/868`, `--cad-monitor-433/868`, `--cad-rssi-433/868` are removed (rejected as unknown options); the plain `--tx-mode`, `--cad-monitor`, `--cad-rssi` apply to the selected band.

- Fixes from the hardware validation (`HW-ONAIR-CHECKLIST.md`): sx1262 RF-switch polarity (TX radiated nothing), sx127x LDRO warm-start ghost on no-RESET boards (garbled RX after reconfig), chip-family-aware FSK `RXBW` validation (RXBW was unconfigurable on SX1262); framed DATA emits one ERROR per rejected frame instead of one per junk byte.

- BEHAVIOR: main-loop lock discipline — monitoring, RX poll, and GETRSSI ticks now SKIP their sample instead of stalling when the TX worker holds the radio across a blocking transmit (seconds at SF12). CONFIG apply deliberately still blocks (client-initiated, must not be dropped). Contract documented in `radio_controller.h`.
- Drivers fail closed on missing band RF defaults: `begin(NULL)`/`switchMode(..., NULL)` return `NULL_POINTER` instead of falling back to chip register defaults (no code path can park a process off-band).
- BEHAVIOR: CAD probe pending-RX guard (audit M1) — a fully received but not yet drained packet is no longer destroyed by the probe's IRQ-clear/re-arm; the probe reports BUSY without scanning, MANAGED TX waits, the packet is delivered.
- BEHAVIOR: `SET MODE` lands on the band's boot RF defaults (audit M2) — `switchMode()` requires the band defaults (fail-closed on NULL) instead of leaving the chip at its 434-MHz register defaults after a mode change.
- BEHAVIOR: RX re-arm robustness (audit M3) — every `startReceive()` return is captured: boot failure fails closed (`RADIO_HEALTH_FAILED` + numeric code), runtime failures log once per incident, count in the new appended `GET STATS` field `RXREARMFAIL`, and are retried each radio tick under try_to_lock until recovery (no READY-but-deaf radio).
- Polish: root-run SKIP guard in the lockdir tests, per-band log tags read from the band descriptor (new CAD/RSSI fields) instead of ternaries, hardware-profile resolve uses the frozen descriptor's band number, v113 bench items appended to `HW-ONAIR-CHECKLIST.md`.
- Low-findings hardening: listening sockets are nonblocking so a reset connection can never hang the daemon in `accept()` (one accept per readiness tick stays the fairness policy); event-fd sync checks registration failure before `reconcile_begin` (no dangling epoch); benign `EINTR` from the event wait no longer hits the `perror` path (debug line only); probe `rssi_dbm` semantics (packet vs live per path) documented at the source.
- Robustness polish: STATS/STATUS reply buffers sized for saturated 20-digit counters (no truncation possible), RX re-arm latch transitions are atomic exchanges, the re-arm retry never runs over an undrained packet, and shutdown resets the re-arm latch.
- BEHAVIOR: every mandatory boot setter (frequency, SF, BW, sync, preamble, CR, CRC, LDRO, power) is now checked — a rejected setter fails the boot closed (FAILED health, stage logged) instead of a READY radio with partial RF configuration.
- BEHAVIOR: `SET FREQ` is validated against the band's operational range from the descriptor (433: 430.0–440.0 MHz, 868: 863.0–870.0 MHz) — an 868 process can no longer be tuned off its logical band.
- BEHAVIOR: preamble ranges are bounded (LoRa 6–512, FSK 0–2048; the old 65535 ceiling allowed one valid ~36-minute preamble at SF12/BW125 — full airtime-based validation deferred to the transactional-CONFIG rework). Duplicate keys in one SET command are rejected (ambiguous under sequential apply).
- SPI bus lock acquisition is deadline-bounded (2 s, CLOCK_MONOTONIC): a live-but-wedged peer process now causes a controlled fatal exit instead of hanging this daemon forever.
- Background-mode log open is symlink-safe (`O_NOFOLLOW`); a symlink planted at the fixed /tmp log path is a hard error. Fixed a signed-shift UB in the RX debug decoder; test harness now fails on missing summaries, summary assertion failures, and unexpected XPASS (fail-open guard).
- Repo entry point: root README now presents `loraham_daemon/` as the canonical tree; the legacy single-file daemons moved to `legacy/` (archived, not buildable per instructions).
- BEHAVIOR: CONFIG apply reports a structured status — unknown/rejected/no-op commands no longer re-arm RX (a malformed CONF line could destroy a received, undrained packet); driver parameter setters return their RadioLib state, apply aborts on the first hardware failure, and a mid-apply failure fails the radio closed (`RADIO=FAILED`).
- BEHAVIOR: MANAGED synchronous TX treats a failed/unavailable CAD probe as an error (abort, `RADIO_ERROR`) instead of a free channel; the passive probe reports UNAVAILABLE for sentinel RSSI readings instead of a false FREE.
- BEHAVIOR: family-aware prevalidation — SX1262 rejects `OOK=1`, `ENCODING=1` (would silently enable whitening), and `FREQDEV<0.6`; duplicate `MODE` keys are rejected like other duplicates.
- BEHAVIOR: persistent RX re-arm failure escalates to `RADIO=FAILED` after 30 consecutive failures; retries back off to 1/s; `GET STATUS` gains the appended `RXREADY` field.
- Raw DATA reads are bounded by free TX-queue capacity (a single 2048-byte read could exceed the 8-job queue and silently drop the tail; excess bytes now stay in the kernel buffer). `TXQ*` STATUS counters report the worker's real state even while `TXQUEUE=0`.
- BEHAVIOR: airtime policy — the daemon tracks the effective SF/BW/CR/preamble (FSK: bitrate/preamble) and rejects any `SET` whose merged worst-case 255-byte airtime exceeds 20 s (below the 30 s systemd stop timeout; a blocking transmit is uninterruptible). Checked before any side effect; `MODE` re-bases on band boot defaults. Real profiles pass with wide margin.
- BEHAVIOR: radio-touching `SET` commands are rejected while queued TX jobs are pending or executing (queued jobs transmit with execution-time configuration; retuning accepted jobs was the hazard) — retry when the queue drains; `GET` and runtime-flag commands always pass.
- BEHAVIOR: cross-process GPIO ownership — one advisory lock per claimed pin (`gpio<N>.lock`, ascending order) acquired before any hardware access; a pin held by another process fails the boot closed (deterministic conflict enforcement independent of lgpio's printed-and-swallowed claim errors).
- sx1262 RF-switch setup (DIO2) is checked and propagated (a failure meant TX_RESULT OK with zero radiated RF); the HAL owns the SPI handle and fails closed on transfer errors (previously printed and swallowed by the base HAL); radio health is an atomic; SPI-lock acquisition documented with its latency trade-off; CADRSSI's dual role (monitor + Uputronics passive-LBT TX gate) documented; band-policy FREQ rejections log a distinct "off-band frequency" reason.


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
