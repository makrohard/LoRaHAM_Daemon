# Limits and timing

This page collects the daemon's public limits, its timing constants, and the TX/CAD, startup and
shutdown behaviour built on them. Every value below says whether it is a compile-time constant or
settable at runtime. The commands that change the settable ones are described in
[CONF protocol](conf-protocol.md), the boot-time flags in [CLI](cli.md), and the frame
layouts the size limits apply to in [DATA protocol](data-protocol.md).

## Contents

- [Size and capacity limits](#size-and-capacity-limits)
- [Timing constants](#timing-constants)
- [Runtime-settable CAD and TX parameters](#runtime-settable-cad-and-tx-parameters)
- [TX modes and the TX queue](#tx-modes-and-the-tx-queue)
- [CAD probes](#cad-probes)
- [The CAD monitor indicator](#the-cad-monitor-indicator)
- [Accepted parameter ranges](#accepted-parameter-ranges)
- [Worst-case airtime ceiling](#worst-case-airtime-ceiling)
- [Startup and shutdown timing](#startup-and-shutdown-timing)

## Size and capacity limits

All values in this table are compile-time constants. None of them can be changed at runtime or
from the command line.

| Constant | Value | Meaning |
|---|---:|---|
| `MAX_CLIENTS` | `10` | Client slots per socket group; there are three groups (raw DATA, framed DATA, CONF), each with its own slot array |
| `buf_SIZE` | `256` bytes | Internal RX buffer and CONF read scratch buffer; it is also the bound on a single CONF request line |
| `DATA_TX_MAX_CHUNK_SIZE` | `255` bytes | Ceiling of the raw DATA chunker; the chunk actually generated is the radio's current limit (below) |
| `FRAMED_DATA_MAX_RF_PAYLOAD` | `255` bytes | Storage ceiling for `TX_PACKET` / `RX_PACKET`; what the radio will accept is the limit below |
| `DAEMON_TX_QUEUE_CAPACITY` | `8` jobs | Per-radio bounded async TX queue |
| `DAEMON_TX_COMPLETION_QUEUE_CAPACITY` | `16` results | Per-band bounded async TX completion queue |
| `LORAHAM_SPI_LOCK_TIMEOUT_MS` | `2000` ms | Bound on acquiring the shared SPI transaction lock |
| `CONFIG_POLICY_MAX_AIRTIME_MS` | `20000.0` ms | Worst-case single-packet airtime a configuration may have |

### RF payload limit

The constants above are **storage** ceilings. What one frame may actually carry depends on the chip
family and the modem:

| Board / mode | Limit |
|---|---:|
| SX127x, LoRa | `255` bytes |
| SX127x, FSK | `63` bytes |
| SX126x, either | `255` bytes |

The SX127x FSK FIFO is 64 bytes and variable-length packet mode puts the length byte into it, so 63
is what fits. A `TX_PACKET` above the limit is rejected as `INVALID_PACKET` before anything is
queued and before the radio is touched; raw DATA input is chunked to the limit instead. The limit
also sets the byte bound on how much is read from a client at once, so the chunker cannot generate
more jobs than the TX queue has room for.

The two queues overflow in opposite directions, and a client that watches only one of them will
draw the wrong conclusion from the other:

- The TX queue is **reject-newest**. A submission into a full queue returns `-1`; no already
  queued packet is discarded.
- The completion queue is **drop-oldest**. A result pushed into a full queue advances the head,
  discarding the oldest stored result, and increments the eviction counter reported as
  `TXQRESULTDROP`.

## Timing constants

| Constant | Value | Meaning | Settable |
|---|---:|---|---|
| `DAEMON_TX_POLICY_BUSY_TIMEOUT_MS` | `120000` ms | How long a direct synchronous DATA TX waits for another TX to finish | Compile-time only |
| `DAEMON_TX_POLICY_CAD_WAIT_TIMEOUT_MS` | `1500` ms | How long `MANAGED` TX waits for the channel | Boot default; `SET CADWAIT` |
| `DAEMON_TX_POLICY_CAD_IDLE_STABLE_MS` | `250` ms | Continuous idle CAD time `MANAGED` TX requires before transmitting | Boot default; `SET CADIDLE` |
| `DAEMON_TX_POLICY_POLL_INTERVAL_MS` | `50` ms | Poll interval of both the TX-busy and the CAD wait loop | Boot default; `SET CADPOLL` |
| `DAEMON_TX_POLICY_SEND_AFTER_CAD_TIMEOUT` | `0` (disabled) | Whether `MANAGED` TX sends anyway after the CAD wait timeout | Boot default; `SET CADTXAFTERTIMEOUT` |
| `DAEMON_EVENT_LOOP_TIMEOUT_USEC` | `10000` µs / `10` ms | Main loop socket wait timeout | Compile-time only |
| `DAEMON_RSSI_INTERVAL_MS` | `100` ms | `GETRSSI=1` stream cadence, about 10 Hz | Compile-time only |
| `DAEMON_CAD_POLL_INTERVAL_MS` | `200` ms | Cadence of the CONF `CAD=1/0` RSSI probe | Compile-time only |
| `RADIO_CAD_RSSI_BUSY_THRESHOLD_DBM` | `-90.0` dBm | Live RSSI at or above which the channel counts as busy | Boot default; `--cad-rssi`, `SET CADRSSI` |
| `DAEMON_CAD_FREE_HYSTERESIS_DB` | `3.0` dB | How far below the threshold a sample must be to confirm free | Compile-time only |
| `DAEMON_CAD_FREE_CONFIRM_SAMPLES` | `2` samples | Consecutive confirming samples before `CAD=0` is published | Compile-time only |
| `DAEMON_RX_REARM_FAIL_LIMIT` | `30` failures | Consecutive failed re-arms that flip the radio to `FAILED` | Compile-time only |
| `DAEMON_RX_REARM_RETRY_MS` | `1000` ms | Backoff between RX re-arm attempts, at most one per second | Compile-time only |
| `DAEMON_STATS_LOG_INTERVAL_MS` | `3600000` ms / 60 min | Cadence of the compact operator stats line | Compile-time only |

`DAEMON_STATS_LOG_INTERVAL_MS` is a fixed constant, not a default: there is no command-line option
and no runtime setter for the stats cadence. Earlier documentation described it as changeable,
which was never true.

## Runtime-settable CAD and TX parameters

These are per band, and each takes effect for the process that owns that band. The command syntax
and the reply contract are in [CONF protocol](conf-protocol.md).

| Setting | Accepted range | Default |
|---|---|---|
| `SET CADWAIT=<ms>` | `50`–`5000` ms | `1500` ms |
| `SET CADIDLE=<ms>` | `0`–`2000` ms | `250` ms |
| `SET CADPOLL=<ms>` | `10`–`500` ms | `50` ms |
| `SET CADRSSI=<dbm>` | `-130`–`0` dBm | `-90` dBm |
| `SET CADTXAFTERTIMEOUT=<0\|1>` | `0` or `1` | `0` |
| `SET CADMONITOR=<0\|1>` | `0` or `1` | `0` |
| `SET TXMODE=DIRECT\|MANAGED` | `DIRECT` or `MANAGED` | `MANAGED` |
| `SET TXQUEUE=<0\|1>` | `0` or `1` | `1` |

`CADRSSI` is not only a display threshold. On wiring without DIO1 it also gates whether `MANAGED`
TX transmits at all; see [CAD probes](#cad-probes).

## TX modes and the TX queue

The default TX mode is `MANAGED`, and the default DATA TX path uses `TXQUEUE=1`. DATA socket
transmissions therefore enter the per-band bounded async worker queue, and CAD/LBT runs inside
that worker before the RF transmit. `SET TXQUEUE=0` keeps the direct DATA TX path.

`MANAGED` waits for the stable-idle CAD window before transmitting. If the CAD wait timeout
expires first, the send returns `CHANNEL_BUSY` and the packet is discarded, unless
`CADTXAFTERTIMEOUT=1` is set, in which case it sends anyway.

`DIRECT` applies no CAD gating: the direct path returns a free verdict without probing, and the
queued path disables CAD for the job entirely. `DIRECT` therefore never returns `CHANNEL_BUSY`
*from CAD gating*. It can still return `CHANNEL_BUSY` from the paths that have nothing to do with
CAD:

| Situation | Outcome | Notes |
|---|---|---|
| `TXQUEUE=1`, async queue full | `CHANNEL_BUSY` | Logged and counted as `TX_RESULT_BUSY`; increments `TXQREJECT` |
| `TXQUEUE=0`, wait for the radio to leave TX-busy times out after `120000` ms | `CHANNEL_BUSY` | Logged and counted as `TX_RESULT_BUSY` |
| `TXQUEUE=0` with residual async work still pending or executing | `BUSY` | Defence in depth: a direct TX must not wait behind or interleave with residual jobs |

The 120 s TX-busy timeout does not produce the framed status `BUSY`. It returns the outcome
`CHANNEL_BUSY`, which maps to framed status `CHANNEL_BUSY` (`2`), not `BUSY` (`1`); only the stats
counter and the log line say `BUSY`. The framed status `BUSY` (`1`) comes from the residual-queue
guard.

A drop-free `DIRECT` needs both settings: `--tx-mode direct` (or `SET TXMODE=DIRECT`) *and*
`SET TXQUEUE=0`. Under the queued default a `DIRECT` job is still submitted to the 8-slot queue
and is rejected when that queue is full.

Older or raw clients open a DATA socket, write, and never issue `SET TXMODE`. Under the `MANAGED`
default their packets can be delayed, or dropped on a CAD timeout. Start such a band in `DIRECT`
with `--tx-mode direct` so send-when-told behaviour is active from boot.

## CAD probes

The daemon has two distinct channel probes.

**Active scan probe.** Calls `scanChannel()` on the driver. It is used only for `MANAGED` TX
gating and for the on-demand `GET CHANNEL` query. Two guards apply:

- An RF packet that finished reception but has not been drained by the main loop yet makes the
  probe report `BUSY` without scanning, so the probe's IRQ-clear and RX re-arm can never destroy a
  pending packet.
- Without DIO1 there is no trustworthy `scanChannel()`. The probe then falls through to the
  passive probe, so `MANAGED` TX gating degrades to passive listen-before-talk against the
  `CADRSSI` threshold, and LBT stays functional on such wiring. On that hardware, changing
  `CADRSSI` changes whether the radio transmits.

**Passive RSSI probe.** Reads the live channel RSSI from the driver only. It never changes radio
mode, never calls `scanChannel()`, and never re-arms RX, so it cannot disturb continuous RX. It is
skipped rather than blocked: it returns `UNAVAILABLE` when TX is busy or when the radio mutex is
not free on a try-lock, and `UNAVAILABLE` means the state was left untouched. The verdict is
`BUSY` when the RSSI is at or above the `CADRSSI` threshold and `FREE` below it. Outside LoRa mode
it returns `UNAVAILABLE`.

## The CAD monitor indicator

The `CAD=1` / `CAD=0` lines on the CONF socket come from the passive probe, not from scan-based
CAD.

- Monitoring runs only when it is opted in per band with `SET CADMONITOR=1` *and* a CONF client is
  connected. Without the opt-in, or with no CONF client, nothing is sampled.
- It runs in LoRa mode only. In FSK the passive probe returns `UNAVAILABLE` and no `CAD` line is
  produced.
- A sample is taken at most every `200` ms.
- `CAD=1` asserts immediately at or above the threshold (default `-90` dBm).
- `CAD=0` is published only after `2` consecutive samples at least `3.0` dB below the threshold,
  about 400 ms at the 200 ms cadence.
- A sample in the dead band between the two levels keeps the currently published state and cancels
  an incomplete free confirmation.
- A pending or newly received RF packet never suppresses or delays the `CAD=0` edge: the monitor
  tick ignores the received flag.

## Accepted parameter ranges

A `SET` outside these ranges is rejected before any hardware is touched.

| Parameter | Accepted values |
|---|---|
| `FREQ` (`--radio 433` process) | `430.0`–`440.0` MHz |
| `FREQ` (`--radio 868` process) | `863.0`–`870.0` MHz |
| `SF` | `7`–`12` |
| `BW` | `7.8`, `10.4`, `15.6`, `20.8`, `31.25`, `41.7`, `62.5`, `125`, `250`, `500` kHz |
| `CR` | `5`–`8` |
| `PREAMBLE` (LoRa) | `6`–`512` symbols |
| `POWER` (SX127x board) | `2`–`17` dBm |
| `POWER` (SX126x board) | `0`–`20` dBm |
| `BR` (FSK) | `0.5`–`300` kbps |
| `FREQDEV` (FSK) | greater than `0` and at most `200` kHz; on an SX126x board additionally at least `0.6` kHz |

A frequency outside the band window is rejected with the distinct reason
`off-band frequency (band policy)`.

The `POWER` window is narrower than the hardware, and it is per chip family. The policy check runs
before `setOutputPower()`.

On an **SX126x** board the chip covers -9 dBm to +22 dBm and the accepted range `0`–`20` dBm lies
inside it.

On an **SX127x** board the accepted range is `2`–`17` dBm, for two separate reasons:

- Below `2` dBm RadioLib drives the **RFO** pin instead of PA_BOOST. That is a different output
  path, and it is not the one the antenna is connected to on these boards, so `POWER=0` would have
  meant "transmit into an unconnected pin" while reporting success.
- `18` and `19` dBm are rejected by RadioLib itself (`checkOutputPower` accepts `2`–`17` on PA_BOOST
  and special-cases exactly `20`), so they were never reachable; rejecting them here only makes the
  error early and specific instead of a late driver code.
- `20` dBm **was** reachable, through the PA_DAC-boosted path, and is dropped deliberately: the
  datasheet restricts it to a duty cycle of at most 1 %, VSWR at most 3:1 and VDD 2.4–3.7 V, and the
  daemon has no duty-cycle governor. This is an **intentionally unsupported** high-power mode, not
  an oversight. If it is ever wanted it returns as a feature with that operating contract attached.

Output power and the PA over-current limit (OCP) are applied together as one setting. RadioLib pins
OCP to 60 mA inside both `begin()` and `beginFSK()`, below the datasheet typical draw of 87 mA at
+17 dBm on PA_BOOST; the daemon sets it to **120 mA** — a project-selected margin above that
operating point, not a Semtech figure — at boot, on every `SET POWER`, and after every LoRa/FSK
switch, because `beginFSK()` re-pins it.

## Worst-case airtime ceiling

The daemon rejects any `SET` whose merged configuration would give a worst-case single-packet
airtime above `CONFIG_POLICY_MAX_AIRTIME_MS`, that is 20 s. Worst case means the largest payload the
daemon can actually send in the prospective mode, with CRC on: 255 bytes in LoRa, and 63 bytes in
FSK on an SX127x board, where the 64-byte FIFO also holds the length byte. The gate runs before any hardware side effect and logs the computed airtime together
with the rejection:

```
[<band>] CONFIG rejected: worst-case airtime <ms> ms > 20000 ms (SF<n>/BW<khz>/CR<n>/PRE<n>, <n> B)
```

The check is made against a merged shadow of the configuration, not against the single key being
set:

- In LoRa the shadow tracks `SF`, `BW`, `CR` and `PREAMBLE`; in FSK it tracks the bitrate and the
  preamble.
- A `MODE` token in the same command re-bases the shadow on the band's boot defaults.

That merge is what makes the outcome depend on the whole line. `SET SF=7 BW=7.8` passes at roughly
9 s, while `SET BW=7.8` alone under an SF12 configuration is rejected at roughly 145 s.

Both per-band boot profiles pass the gate: about 9.0 s at 433 (SF12/BW125/CR5, preamble 8) and
about 2.2 s at 868 (SF11/BW250/CR5, preamble 16).

The 20 s ceiling deliberately sits below the `TimeoutStopSec=30` of the shipped systemd unit, so
an in-flight packet cannot outlive a stop request. See [Deployment](deployment.md).

## Startup and shutdown timing

**SPI lock.** Every SPI transaction acquires the shared lock by polling `flock(LOCK_EX|LOCK_NB)`
at a 1 ms interval against a `CLOCK_MONOTONIC` deadline of `LORAHAM_SPI_LOCK_TIMEOUT_MS` (2000
ms). Expiry is a controlled fatal; the daemon never proceeds unlocked.

**RX re-arm.** A failed re-arm must not leave a radio that reports ready but hears nothing.

- At boot the check fails closed: a failed `startReceive()` marks the radio `FAILED` with the
  numeric RadioLib code.
- At runtime a failure is logged once per incident, increments the `rx_rearm_failures` counter and
  latches a pending flag; a later successful re-arm logs the recovery and clears the latch.
- Retries run in the main loop's radio tick, gated on TX-busy and a try-lock, at most one SPI
  attempt per second.
- `DAEMON_RX_REARM_FAIL_LIMIT` (30) consecutive failed re-arms flip the radio to `FAILED`.

**Operator stats.** Every 60 minutes the daemon prints one compact stats line for the selected
radio, carrying the same fields as `GET STATS`.

**Shutdown.** Stopping the async TX worker discards the jobs still queued, rejects any new
submission, and lets an already dequeued job run to completion before the thread is joined.
Cleanup then proceeds in a fixed order: radios stopped, event backend closed, client connections
closed, socket files removed, and the per-band instance lock released last, so a same-band restart
cannot bind sockets this instance is about to delete.
