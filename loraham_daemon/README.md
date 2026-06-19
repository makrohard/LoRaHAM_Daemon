# LoRaHAM_Daemon

`loraham_daemon` is the local hardware daemon for LoRaHAM_Pi / LoRaHAM Cartridge on Raspberry Pi. By default it initializes both the 433 MHz and 868 MHz radio modules, but it can also run either radio independently via `--radio 433` or `--radio 868`. Active radios are exposed to user programs through local UNIX sockets.

The daemon is the interface between the LoRaHAM radio hardware and applications such as LoRaHAM iGate, PiGate, LoRaHAM Chat, test tools, or custom clients. External bridge/client programs can use these sockets to integrate APRS or other higher-level protocols.

## Purpose

- Control the LoRaHAM radio hardware from user space, either as dual-radio daemon or as selected single-radio daemon.
- Provide raw DATA sockets for backward-compatible stream TX/RX.
- Provide framed DATA sockets for packet-boundary-preserving clients.
- Provide CONF sockets for runtime radio configuration.
- Keep radio access centralized so client programs do not need direct SPI/RadioLib access.
- Print live RX/TX/debug information when run in the foreground.

## Current architecture

### Process and lifecycle

| Area | File/module | Role |
|---|---|---|
| Main daemon / orchestration | `loraham_daemon.cpp` | Process entry point, CLI/startup, runtime context, main loop, and high-level coordination of I/O, socket dispatch, radio polling, monitoring, and shutdown |
| Public daemon constants | `daemon_protocol.h` | Public socket paths, buffer size, client limit, and CAD timing constants |
| Version | `daemon_version.h` | Single source for the daemon version printed by `--version` and at startup |
| Logging | `daemon_log.cpp`, `daemon_log.h` | Normal and debug logging helpers used by daemon/runtime modules |
| Timing | `daemon_timing.cpp`, `daemon_timing.h` | Monotonic timing helpers, deadline timers, RSSI cadence, and periodic stats timing |
| Lifecycle | `daemon_lifecycle.cpp`, `daemon_lifecycle.h` | SIGPIPE handling, daemon background mode, inherited-fd cleanup, stop flag handling, and signal-based shutdown |

### Radio hardware/runtime

| Area | File/module | Role |
|---|---|---|
| Radio selection | `daemon_radio_selection.cpp`, `daemon_radio_selection.h` | Parses and exposes the selected radio mode: `both`, `433`, or `868`; default is `both` |
| Radio controller state | `radio_controller.h` | Per-band RadioLib/HAL/Module ownership, radio health/mode flags, RX callback state, TX/CAD/RSSI flags, LED pin, and RX drop counter |
| Radio runtime | `daemon_radio_runtime.cpp`, `daemon_radio_runtime.h` | Per-radio controller setup/shutdown, RX callback glue, selected-radio readiness, and active-radio logging |
| Radio startup/init | `daemon_radio_init.cpp`, `daemon_radio_init.h` | RadioLib object creation, default radio parameters, callback install, and initial RX start |
| Radio health | `radio_health.cpp` | Radio readiness/failed-state helpers used to guard CONFIG/TX behavior |
| LED/GPIO helpers | `daemon_led.cpp`, `daemon_led.h` | Raspberry Pi GPIO LED setup and per-radio LED blink helpers |

### I/O, sockets, and clients

| Area | File/module | Role |
|---|---|---|
| Daemon I/O runtime | `daemon_io_runtime.cpp`, `daemon_io_runtime.h` | Owns socket fds, client slots, framed client state, per-band channel state, I/O startup/cleanup, and event-fd registration |
| Radio channel I/O | `radio_channel.cpp`, `radio_channel.h` | Per-band raw DATA, framed DATA, and CONF socket descriptors, socket setup/open helpers, client accept/flush flow, and live RSSI helper |
| Event loop | `event_loop.cpp`, `event_loop_epoll.cpp` | Backend-neutral event-loop wrapper plus current epoll implementation for socket readiness |
| UNIX sockets | `unix_socket.cpp` | Create, bind, listen, close, and remove local UNIX socket files; stale socket paths are replaced, non-socket path collisions are rejected |
| Socket runtime | `daemon_socket_runtime.cpp`, `daemon_socket_runtime.h` | Logged per-channel accept/flush helpers around socket client slots |
| Socket dispatch | `daemon_socket_dispatch.cpp`, `daemon_socket_dispatch.h` | Per-band ready-socket orchestration for raw DATA, framed DATA, CONFIG, accept, and flush flow |
| Client handling | `client_output_queue.cpp`, `client_set.cpp`, `client_slot.cpp` | Client slots, nonblocking I/O, queued output, disconnect cleanup, and broadcast helpers |

### DATA/RX/TX runtime

| Area | File/module | Role |
|---|---|---|
| Raw DATA TX | `data_tx.cpp` | Reads raw DATA sockets and splits client writes into RF-sized chunks |
| Shared DATA TX runtime | `daemon_data_tx_runtime.cpp`, `daemon_data_tx_runtime.h` | Applies radio-health checks, TX-busy policy, CAD policy, TX queue selection, TX result state, stats, and DATA TX logging |
| TX policy | `daemon_tx_policy.h` | Central TX-busy timeout, CAD wait timeout, stable-idle window, poll interval, and send-after-CAD-timeout policy |
| TX executor/outcome | `daemon_tx_executor.h`, `daemon_tx_outcome.h`, `daemon_tx_job.h` | Internal TX job/result structures, TX result mapping, and the RadioLib send seam |
| TX queue / worker | `daemon_tx_queue.h`, `daemon_tx_worker.h`, `daemon_tx_async_worker.*`, `daemon_tx_async_runtime.*` | Bounded reject-newest TX queue, per-radio async worker lifecycle, per-band worker counters, and queued completion storage |
| TX completion bridge | `daemon_tx_completion.h` | Encodes final async TX results as framed `TX_RESULT`, targets the originating framed slot, and drops stale completions using client-slot generation |
| Radio TX path | `daemon_tx.cpp`, `daemon_tx.h` | Validates TX requests, broadcasts `TX=1/0` on CONF sockets, prepares/restores radio TX state, and maps RadioLib TX results |
| Framed DATA protocol | `framed_data.cpp`, `framed_data_tx.cpp` | Binary frame helpers, framed TX stream state, ERROR frames, TX_RESULT frames, and RX_PACKET framing |
| Framed DATA runtime | `daemon_framed_data_runtime.cpp`, `daemon_framed_data_runtime.h` | Framed DATA socket read loop, TX_PACKET forwarding, immediate/final TX_RESULT behavior, ERROR handling, and async completion draining |
| RX runtime | `daemon_rx.cpp`, `daemon_rx.h` | Per-band RX packet read/validate/print/forward flow for raw and framed clients |
| RF packet / TX result | `rf_packet.cpp`, `tx_result.cpp` | RF payload validation/preview helpers and normalized TX result states |

### CONFIG and monitoring

| Area | File/module | Role |
|---|---|---|
| CONFIG stream/parser/apply | `config_stream.cpp`, `config_parser.cpp`, `config_value.cpp`, `config_policy.cpp`, `config_validate.cpp`, `config_apply.cpp`, `config_dispatch.h` | Line framing, strict parsing, validation policy, transactional apply, and dispatch support for `SET KEY=VALUE` commands |
| CONFIG runtime | `daemon_config_runtime.cpp`, `daemon_config_runtime.h` | Build per-band CONFIG dispatch contexts and debug logging callbacks |
| Monitoring runtime | `daemon_monitoring.cpp`, `daemon_monitoring.h` | CAD status, live RSSI streaming, GETRSSI auto-stop, and periodic operator stats |

## Build and test scripts

Build prerequisites: C++ compiler, lgpio development headers, RadioLib source/build tree, and pthread support.

| Command | Meaning |
|---|---|
| `./build.sh` | Build the production daemon binary |
| `./build.sh --debug` | Build with debug flags |
| `./build.sh --clean` | Remove the daemon binary |
| `./build.sh --radiolib-dir DIR` | Use an explicit RadioLib source/build tree |
| `./run_tests.sh` | Build daemon/tests and run the normal non-RF test suite |
| `./run_tests.sh --TX` | Also run optional RF transmit smoke tests |
| `./run_tests.sh --TX --rx-seconds N` | Set RF TX/RX observation time; default is `15` seconds |

`--TX` requires free radio hardware, SPI/GPIO access, and a safe RF test setup. Use the default `./run_tests.sh` for normal non-RF checks.

## Daemon command line

| Option | Default | Accepted values | Meaning |
|---|---:|---|---|
| none | foreground | - | Run in terminal and print traffic/debug output |
| `-d`, `--daemon` | off | `-d`, `--daemon` | Run in background; stdout/stderr go to `/tmp/lora_daemon.log` |
| `-v`, `--version` | - | `-v`, `--version` | Print daemon version and exit |
| `--debug` | off | `--debug` | Enable debug log output |
| `--radio MODE` | `both` | `both`, `433`, `868` | Select active radio backend: both radios, 433 MHz only, or 868 MHz only |
| `-h`, `--help` | - | `-h`, `--help` | Print usage and exit |

## UNIX socket interface

| Socket | Type | Direction | Payload | Meaning |
|---|---|---|---|---|
| `/tmp/lora433.sock` | raw DATA 433 | client ↔ daemon | raw bytes | TX raw bytes on 433 MHz; receive raw RF packets from 433 MHz |
| `/tmp/lora868.sock` | raw DATA 868 | client ↔ daemon | raw bytes | TX raw bytes on 868 MHz; receive raw RF packets from 868 MHz |
| `/tmp/lora433f.sock` | framed DATA 433 | client ↔ daemon | binary frames | TX/RX packet-preserving frames on 433 MHz |
| `/tmp/lora868f.sock` | framed DATA 868 | client ↔ daemon | binary frames | TX/RX packet-preserving frames on 868 MHz |
| `/tmp/loraconf433.sock` | CONF 433 | client ↔ daemon | text commands | Configure 433 MHz radio; receive CONF status/events |
| `/tmp/loraconf868.sock` | CONF 868 | client ↔ daemon | text commands | Configure 868 MHz radio; receive CONF status/events |

Socket availability depends on the selected radio mode:

| Radio mode | Created sockets |
|---|---|
| default / `--radio both` | all six raw DATA, framed DATA, and CONF sockets |
| `--radio 433` | `/tmp/lora433.sock`, `/tmp/lora433f.sock`, `/tmp/loraconf433.sock` |
| `--radio 868` | `/tmp/lora868.sock`, `/tmp/lora868f.sock`, `/tmp/loraconf868.sock` |

Inactive-radio sockets are not created. This is intentional so clients can detect which radio backend is active by checking the socket path.

UNIX socket setup rejects existing non-socket filesystem entries at the public socket paths. Existing stale UNIX socket files are replaced.

## Public limits and timing

| Item | Default/current value | Meaning |
|---|---:|---|
| `MAX_CLIENTS` | `10` | Client slots per socket group |
| `buf_SIZE` | `256` bytes | Internal RX/CONFIG buffer size |
| Raw DATA TX chunk | `255` bytes | Maximum RF chunk generated from raw DATA socket input |
| Framed DATA RF payload | `255` bytes | Maximum RF payload accepted for `TX_PACKET` and carried inside `RX_PACKET` |
| Framed RX metadata | `4` bytes | `int16` RSSI c-dBm + `int16` SNR c-dB before RF bytes |
| Framed RX frame | `262` bytes | Maximum complete `RX_PACKET`: 3-byte header + 4-byte metadata + 255 RF bytes |
| TX queue capacity | `8` jobs | Per-radio bounded async TX queue, reject-newest when full |
| TX completion queue capacity | `16` results | Per-band bounded async completion queue, drop-oldest when full |
| TX-busy wait timeout | `120000 ms` / `120 s` | Direct synchronous DATA TX waits this long for another TX to finish before returning BUSY |
| CAD wait timeout | `20000 ms` / `20 s` | MANAGED TX waits this long for channel availability before applying timeout policy |
| CAD stable-idle window | `500 ms` | MANAGED TX requires this much continuous idle CAD time before TX |
| TX/CAD policy poll interval | `100 ms` | Poll interval used by TX-busy and CAD wait loops |
| Send after CAD timeout | enabled | MANAGED TX sends after CAD timeout and marks the final result with the CAD-timeout flag |
| Event-loop timeout | `10000 µs` / `10 ms` | Main loop socket wait timeout |
| RSSI interval | `100 ms` | `GETRSSI=1` stream cadence, about 10 Hz |
| CAD monitor poll interval | `30` loop ticks | CONF `CAD=1/0` monitoring cadence |

## Current TX/CAD behavior

The default TX mode is `MANAGED`. The default DATA TX path uses `TXQUEUE=1`, so DATA socket transmissions enter the per-band bounded async worker queue and CAD/LBT runs in that worker before RF transmit. `SET TXQUEUE=0` keeps the legacy direct DATA TX path available. `RAW` mode performs one CAD probe before TX and blocks when the channel is busy. `MANAGED` mode waits for the stable-idle CAD window, sends after the CAD timeout when configured, and marks that final framed `TX_RESULT` with the CAD-timeout flag. Deferred framed `TX_RESULT` delivery targets the originating framed client slot and is dropped if that slot was closed or reused before completion.

## DATA sockets

DATA sockets are raw byte streams. They have no line protocol and no length prefix.

| Action | Behavior |
|---|---|
| Client writes bytes to DATA socket | Daemon transmits them on the matching band |
| Write is larger than 255 bytes | Daemon splits it into multiple RF chunks |
| RF packet is received | Daemon broadcasts raw bytes to connected DATA clients of that band |
| Client disconnects | Daemon closes the slot and can reuse it |

Examples:

```bash
printf 'Hello LoRaHAM\n' | socat - UNIX-CONNECT:/tmp/lora433.sock
```

```bash
echo "FFFFFFFF67452301FFBFC1E80700000028505C33DC22F5051C" \
  | perl -pe 's/([0-9A-Fa-f]{2})/chr(hex($1))/ge' \
  | socat - UNIX-CONNECT:/tmp/lora868.sock
```

## Framed DATA sockets

Framed DATA sockets are opt-in binary stream sockets for clients that need stable
RF packet boundaries. Raw DATA sockets remain unchanged and are still the
backward-compatible default interface.

Frame layout:

| Offset | Size | Meaning |
|---:|---:|---|
| `0` | 1 byte | frame type |
| `1` | 2 bytes | payload length, little-endian `uint16` |
| `3` | `length` bytes | payload |

Frame types:

| Type | Name | Direction | Meaning |
|---:|---|---|---|
| `0x01` | `RX_PACKET` | daemon → client | One complete received RF packet with RSSI/SNR metadata |
| `0x02` | `TX_PACKET` | client → daemon | One complete RF packet to transmit |
| `0x03` | `ERROR` | daemon → client | UTF-8 error text |
| `0x04` | `TX_RESULT` | daemon → client | Per-TX result payload when enabled with `SET TXRESULT=1` on the matching CONF socket |

`RX_PACKET` payload layout:

| Payload offset | Size | Type | Meaning |
|---:|---:|---|---|
| `0` | 2 bytes | little-endian `int16` | RSSI in centi-dBm, or `-32768` if unavailable |
| `2` | 2 bytes | little-endian `int16` | SNR in centi-dB, or `-32768` if unavailable; FSK uses unavailable |
| `4` | `rf_len` bytes | bytes | received RF payload, maximum `255` bytes |

`TX_RESULT` payload layout:

| Payload offset | Size | Type | Meaning |
|---:|---:|---|---|
| `0` | 1 byte | `uint8` | status: `0` OK, `1` BUSY, `2` CHANNEL_BUSY, `3` RADIO_NOT_READY, `4` RADIO_ERROR, `5` INVALID_PACKET, `6` INVALID_BAND |
| `1` | 1 byte | bit mask | flags: bit `0` managed-mode attempt, bit `1` deferred/final queued result, bit `2` MANAGED send-after-CAD-timeout |
| `2` | 2 bytes | little-endian `uint16` | sequence number |

Rules:

- `TX_PACKET` payloads must be at most `255` RF bytes and contain no metadata.
- `TX_RESULT` payload length is exactly `4` bytes.
- `SET TXRESULT=1` and `SET TXRESULT=0` on the matching CONF socket enable or disable per-band `TX_RESULT` emission.
- `SET TXMODE=MANAGED` and `SET TXMODE=RAW` select per-band TX mode; default is `MANAGED`.
- `SET TXQUEUE=1` routes DATA TX through the per-band bounded async TX queue; `SET TXQUEUE=0` keeps direct DATA TX.
- `GET STATUS` reports `TXRESULT`, `TXMODE`, `TXQUEUE`, queue counters, last queued TX result, and last queued TX sequence.
- `GET CHANNEL` returns a one-line per-band snapshot with `RADIO`, `BUSY`, `CAD`, `RSSI`, `MODE`, and `TXMODE`.
- `RAW` TX mode performs one CAD probe and returns `CHANNEL_BUSY` when the channel is busy.
- `MANAGED` TX mode waits for stable CAD idle before TX and sends after the CAD timeout when the policy allows it.
- Final framed `TX_RESULT` flags include managed/deferred/CAD-timeout context.
- Queued framed TX suppresses the immediate success `TX_RESULT`; final async completion is delivered later to the originating framed client.
- If the originating framed client slot has closed or been reused before completion, the stale final `TX_RESULT` is dropped and counted in `TXQSTALE`.
- Oversized `TX_PACKET` frames and unsupported frame types are rejected with an `ERROR` frame.
- One valid `TX_PACKET` maps to one RF transmit attempt; it is not split.
- One received RF packet is sent to framed clients as exactly one `RX_PACKET`.
- `RX_PACKET` payload length is `4 + rf_len`; maximum complete RX frame size is `262` bytes.
- Raw DATA clients continue to receive raw RF bytes exactly as before.
- Raw and framed DATA sockets of the same band share the same radio backend.
- Multiple clients may connect to the same band, but TX arbitration is best-effort/event-loop ordered.
- Clients that need exclusive radio use must coordinate externally.
- Framed DATA is packet transport only; higher-level protocols stay in clients.

Minimal Python RX frame reader:

```python
import socket
import struct

SIGNAL_UNAVAILABLE = -32768

def recv_exact(sock, n):
    data = bytearray()
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise EOFError("socket closed")
        data += chunk
    return bytes(data)

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect("/tmp/lora868f.sock")

header = recv_exact(sock, 3)
frame_type, payload_len = struct.unpack("<BH", header)
payload = recv_exact(sock, payload_len)

if frame_type == 0x01:
    rssi_cdbm, snr_cdb = struct.unpack("<hh", payload[:4])
    rf_payload = payload[4:]
    rssi = None if rssi_cdbm == SIGNAL_UNAVAILABLE else rssi_cdbm / 100.0
    snr = None if snr_cdb == SIGNAL_UNAVAILABLE else snr_cdb / 100.0
    print(rssi, snr, rf_payload)
```

Minimal Python TX frame sender:

```python
import socket
import struct

payload = b"Hello LoRaHAM"
frame = struct.pack("<BH", 0x02, len(payload)) + payload

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect("/tmp/lora868f.sock")
sock.sendall(frame)
```

## CONF sockets

CONF sockets accept text commands:

```text
SET KEY=VALUE KEY=VALUE ...
SET TXRESULT=0|1
SET TXMODE=MANAGED|RAW
SET TXQUEUE=0|1
GET STATUS
GET STATS
GET CHANNEL
```

`GET STATUS` returns one runtime snapshot on the same CONF socket:

```text
STATUS RADIO=READY|FAILED|UNINITIALIZED TX=0|1 CAD=0|1 GETRSSI=0|1 TXRESULT=0|1 TXMODE=MANAGED|RAW TXQUEUE=0|1 TXQ=N TXQDROP=N TXQSTALE=N TXQDONE=N TXQLAST=NAME TXQSEQ=N
```

DATA and framed DATA sockets are payload-only. Status/config messages are never
sent on DATA or framed DATA sockets.

Important behavior:

- CONF input is line-framed: one newline-terminated line is one command.
- One-shot clients that close after a final unterminated command are still accepted for compatibility.
- Keys are parsed case-insensitively.
- Values are parsed strictly: malformed suffixes, empty values, `nan`, `inf`, and partial numbers are rejected.
- The complete CONFIG command is validated before side effects start; invalid commands do not change `MODE`, `GETRSSI`, or radio parameters.
- Malformed tokens such as `BROKEN`, `NOVALUE=`, or `=BAD` reject the whole CONFIG command.
- `MODE=` is applied before other radio parameters.
- `GETRSSI=` is handled directly.
- LoRa-only keys are ignored in FSK mode.
- FSK-only keys are ignored in LoRa mode.
- `GET STATUS`, `GET STATS`, and `GET CHANNEL` return stable one-line responses on the requesting CONF socket.
- `SET TXRESULT=0|1`, `SET TXMODE=MANAGED|RAW`, and `SET TXQUEUE=0|1` are stable per-band control commands.
- `SET` commands and malformed commands do not have a stable OK/ERR response protocol; errors are logged by the daemon.
- `MODE=LORA` calls RadioLib `begin()`.
- `MODE=FSK` calls RadioLib `beginFSK()`.
- After mode reinitialization, the packet-received callback is restored and RX is restarted.

## Startup and shutdown behavior

Startup is selected-radio aware:

- default / `--radio both` requests both radios
- `--radio 433` requests only the 433 MHz radio
- `--radio 868` requests only the 868 MHz radio
- socket setup failure for a selected radio is fatal
- event-loop setup failure is fatal
- signal-handler setup failure is fatal
- if no selected radio becomes ready, the daemon exits
- in default / `--radio both` mode, the daemon may continue if one radio becomes ready and the other one fails
- on shutdown, only selected/created radio sockets and client slots are cleaned up

Normal logs report the active radios once during startup. Debug logs contain more detailed selected-radio decisions.

## Startup defaults

| Band | Mode | FREQ | SF | BW | CR | CRC | PREAMBLE | SYNC | LDRO | POWER |
|---|---|---:|---:|---:|---:|---:|---:|---|---|---:|
| 433 | LORA | `433.900` | `12` | `125` | `5` | `1` | `8` | `0x12` | `1` | `10` |
| 868 | LORA | `869.525` | `11` | `250` | `5` | `1` | `16` | `0x2B` | `AUTO` | `10` |

## CONFIG parameters

| Key | Mode | Default | Accepted values | Common values | Meaning |
|---|---|---|---|---|---|
| `MODE` | global | `LORA` | `LORA`, `FSK` | `LORA`, `FSK` | Select RadioLib mode |
| `FREQ` | LORA/FSK | 433: `433.900`, 868: `869.525` | strict number > `0`, MHz; RadioLib rejects unsupported bands | `433.775`, `433.900`, `869.525` | RF frequency |
| `POWER` | LORA/FSK | `10` | integer `0` to `20` dBm | `10`, `17`, `20` | TX power |
| `GETRSSI` | global | `0` | exact `0`, `1` | `1` start, `0` stop | Stream live RSSI to CONF clients |
| `SF` | LORA | 433: `12`, 868: `11` | integer `7` to `12` | `12`, `11`, `10`, `7` | LoRa spreading factor |
| `BW` | LORA | 433: `125`, 868: `250` | exact `7.8`, `10.4`, `15.6`, `20.8`, `31.25`, `41.7`, `62.5`, `125`, `250`, `500` kHz | `125`, `250` | LoRa bandwidth |
| `CR` | LORA | `5` | integer `5` to `8` | `5` | LoRa coding rate (`4/5` to `4/8`) |
| `CRC` | LORA | `1` | exact `0`, `1` | `1` | LoRa CRC off/on |
| `PREAMBLE` | LORA/FSK | 433: `8`, 868: `16` | LoRa: integer `6` to `65535`; FSK: integer `>=0` | `8`, `16`, `32` | Preamble length |
| `SYNC` | LORA/FSK | 433: `0x12`, 868: `0x2B` | LoRa: `0x00` to `0xFF` or decimal `0` to `255`; FSK: 1 or 2 non-zero bytes, max `0xFFFF` | `0x12`, `0x2B`, `0x2DD4` | Sync word |
| `LDRO` | LORA | 433: `1`, 868: `AUTO` | exact `AUTO`, `auto`, `0`, `1` | `AUTO`, `1` for SF12/BW125 | Low Data Rate Optimization |
| `BR` | FSK | RadioLib/default unset by daemon | strict number `0.5` to `300.0` kbps | `1.2`, `2.4`, `4.8`, `9.6`, `100` | FSK bitrate |
| `FREQDEV` | FSK | RadioLib/default unset by daemon | strict number `>0` to `200.0` kHz; RadioLib may reject invalid BR/FREQDEV combinations | `2.2`, `3.0`, `5.0`, `10.0` | Frequency deviation |
| `RXBW` | FSK | RadioLib/default unset by daemon | exact `2.6`, `3.1`, `3.9`, `5.2`, `6.3`, `7.8`, `10.4`, `12.5`, `15.6`, `20.8`, `25.0`, `31.25`, `31.3`, `41.7`, `50.0`, `62.5`, `83.3`, `100.0`, `125.0`, `166.7`, `200.0`, `250.0` kHz | `6.3`, `12.5`, `20.8`, `25.0`, `250.0` | RX filter bandwidth |
| `OOK` | FSK | RadioLib/default | exact `0`, `1` | `0` FSK, `1` OOK | Enable OOK mode |
| `SHAPING` | FSK | RadioLib/default | exact `off`, `none`, `0.0`, `0.3`, `0.5`, `0.7`, `1.0` | `0.0`, `0.3`, `0.5`, `1.0` | Data shaping / Gaussian filter |
| `ENCODING` | FSK | RadioLib/default | integer `0`, `1`, `2` | `0` NRZ, `1` Manchester, `2` Whitening | FSK encoding |

### CONF queries and runtime stats

CONF sockets also accept read-style query commands. Known queries return one
newline-terminated response line. Unknown or malformed commands keep the legacy
behavior: the daemon logs them but does not send a stable OK/ERR response.

| Command | Response | Meaning |
|---|---|---|
| `GET STATUS` | `STATUS RADIO=READY TX=0 CAD=0 GETRSSI=0 TXRESULT=0 TXMODE=MANAGED TXQUEUE=0 TXQ=0 TXQDROP=0 TXQSTALE=0 TXQDONE=0 TXQLAST=NONE TXQSEQ=0` | Current radio health, runtime flags, TX mode, and TX queue state |
| `GET STATS` | `STATS UPTIME=123 RADIO=READY RX=0 RXBYTES=0 RXDROPS=0 TXOK=0 TXERR=0 TXBUSY=0 CADTIMEOUT=0 CADSEND=0` | Counters since daemon start |
| `GET CHANNEL` | `CHANNEL RADIO=READY BUSY=0 CAD=1 RSSI=-87.50 MODE=LORA TXMODE=MANAGED` | Current channel probe snapshot |

The daemon also prints one compact operator stats line per selected radio every
60 minutes by default. This terminal log uses the same fields as `GET STATS`.

## CONF output messages

| Message | Socket | Meaning |
|---|---|---|
| `CAD=1\n` | matching CONF socket | LoRa channel activity detected |
| `CAD=0\n` | matching CONF socket | LoRa channel no longer active |
| `RSSI=-87.50\n` | matching CONF socket | Live RSSI while `GETRSSI=1` is active |
| `TX=1\n` | matching CONF socket | Local radio transmit started |
| `TX=0\n` | matching CONF socket | Local radio transmit finished |
| `STATUS RADIO=... TX=... CAD=... GETRSSI=... TXRESULT=... TXMODE=... TXQUEUE=... TXQ=... TXQDROP=... TXQSTALE=... TXQDONE=... TXQLAST=... TXQSEQ=...\n` | requesting CONF socket | Reply to `GET STATUS` |
| `STATS UPTIME=... RADIO=... RX=... RXBYTES=... RXDROPS=... TXOK=... TXERR=... TXBUSY=... CADTIMEOUT=... CADSEND=...\n` | requesting CONF socket | Reply to `GET STATS` |
| `CHANNEL RADIO=... BUSY=... CAD=... RSSI=... MODE=... TXMODE=...\n` | requesting CONF socket | Reply to `GET CHANNEL` |
| log: `kein Client mehr verbunden -> GETRSSI auto-stop` | daemon stdout/log | RSSI stream stopped because no CONF client is connected |

`GETRSSI=1` automatically stops when no CONF client remains connected. A reconnect must send `SET GETRSSI=1` again.

## Examples

433 MHz LoRa/APRS-style setup:

```bash
echo "SET MODE=LORA FREQ=433.900 SF=12 BW=125 CR=5 CRC=1 PREAMBLE=8 SYNC=0x12 LDRO=1 POWER=17" \
  | socat - UNIX-CONNECT:/tmp/loraconf433.sock
```

868 MHz LoRa setup:

```bash
echo "SET MODE=LORA FREQ=869.525 SF=11 BW=250 CR=5 CRC=1 PREAMBLE=16 SYNC=0x2B LDRO=AUTO POWER=10" \
  | socat - UNIX-CONNECT:/tmp/loraconf868.sock
```

433 MHz FSK setup:

```bash
echo "SET MODE=FSK FREQ=433.775 BR=4.8 FREQDEV=5.0 RXBW=12.5 POWER=10" \
  | socat - UNIX-CONNECT:/tmp/loraconf433.sock
```

433 MHz OOK setup:

```bash
echo "SET MODE=FSK FREQ=433.920 BR=1.2 RXBW=6.3 OOK=1 POWER=10" \
  | socat - UNIX-CONNECT:/tmp/loraconf433.sock
```

RSSI stream:

```bash
echo "SET GETRSSI=1" | socat - UNIX-CONNECT:/tmp/loraconf433.sock
echo "SET GETRSSI=0" | socat - UNIX-CONNECT:/tmp/loraconf433.sock
```

CONF status and stats queries:

```bash
echo "GET STATUS" | socat - UNIX-CONNECT:/tmp/loraconf433.sock
echo "GET STATS"  | socat - UNIX-CONNECT:/tmp/loraconf433.sock
```

## Warnings

This software is experimental and used at your own risk. Use only for amateur radio or laboratory operation. Frequencies, power levels, and modes must match your license and local regulations.

## Credits and license

Copyright (c) 2020-2026 Alexander Walter / LoRaHAM.  
Refactored by Johannes Loose / 410733@gmail.com in 2026  

The original project is licensed under GNU GPL v3 with additional conditions stated by the author:

- private/hobby use is free
- commercial use requires written permission / commercial license
- modifications should be reported to the author, preferably via pull request
- binaries may only be redistributed with the full source code
- no warranty; use at your own risk
