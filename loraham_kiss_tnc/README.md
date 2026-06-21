# LoRaHAM KISS TNC Bridge
written by Johannes Loose 410733@gmail.com

KISS/TCP TNC bridge for `LoRaHAM_Daemon`.

It exposes a KISS/TCP port for APRS clients and talks to the LoRaHAM daemon through its framed DATA Unix socket.

```text
APRS client <-> KISS/TCP <-> loraham_kiss_tnc <-> /tmp/lora433f.sock <-> loraham_daemon
```

## Compatibility

- requires `loraham_daemon` 110 or newer
- uses framed DATA sockets for daemon packet I/O
- daemon 110 `RX_PACKET` RSSI/SNR metadata is stripped before KISS output
- `TX_PACKET` payloads are still sent as RF bytes without metadata
- daemon 111 `TX_RESULT` is used internally after `STATUS` confirms `TXRESULT=1`
- KISS remains fire-and-forget; TX result frames are never forwarded

## Limitations
- single KISS/TCP client
- KISS port 0 only
- APRS/TNC2 text payload only
- TX packets are queued and sent one at a time
- mode=LORA only - because FSK is not used in LoRa APRS
- framed DATA socket only for daemon packet I/O
- requires `loraham_daemon` 110+ framed RX metadata layout
- RX RSSI/SNR metadata is ignored and not forwarded to KISS clients
- daemon 110 uses CONF events (`TX=`, `CAD=`, `STATUS`) as legacy TX fallback
- daemon 111 waits for terminal `TX_RESULT` before RX frequency restore

## TX policy

- queued TX waits for fresh `STATUS` after CONF reconnect
- `TX=1` delays TX until `TX=0`; timeout drops the packet
- daemon 110 performs the final CAD gate before RF transmit
- CAD daemon status is retained for stats output
- bridge-side CAD wait/ignore options were removed; daemon 110 owns channel gating

The bridge intentionally does not requeue daemon `CAD_TIMEOUT` framed errors. The framed DATA protocol has no TX/error correlation, so requeueing could duplicate or misassign APRS packets. There are no bridge-side `--cad-*` policy options.

## Build

```bash
./loraham_kiss_tnc/build.sh
```

## Test

```bash
./loraham_kiss_tnc/run_tests.sh
```

## Run

```bash
./loraham_kiss_tnc/loraham_kiss_tnc --help
```

## Usage

```bash
Usage: ./loraham_kiss_tnc/loraham_kiss_tnc [OPTIONS]

LoRaHAM KISS/TCP TNC bridge

Options:
  -c, --config FILE        Load config file
      --kiss-host HOST     KISS/TCP bind host
      --kiss-port PORT     KISS/TCP bind port
      --data-socket PATH   LoRaHAM framed data socket
      --conf-socket PATH   LoRaHAM config socket
      --rx-freq MHz        RX/config frequency
      --tx-freq MHz        TX/config frequency
      --rx-only            Disable TX
      --tx-settle-ms MS    Wait after TX freq switch
      --tx-return-ms MS    Fallback wait after TX before RX restore
      --tx-busy-timeout-ms MS
                            Max wait for local TX busy
      --tx-queue-len N     Queued TX packet limit
      --tx-packet-ttl-ms MS
                            Max queued packet lifetime
  -v, --verbose            Verbose output
      --version            Print version and exit
  -h, --help               Show help

```

## Serial KISS

Xastir only supports Serial KISS TNC. You can use socat to forward the byte-stream to a PTY.
This works over network, too.

Start socat on the machine with the APRS-Client and point to the machine running the TNC.

```bash
socat -d -d \
  PTY,link=/tmp/loraham_kiss,raw,echo=0,waitslave \
  TCP:127.0.0.1:8001
```


Make sure that the TNC listens on the network. For testing, you may use 0.0.0.0.
```bash
./loraham_kiss_tnc/loraham_kiss_tnc \
  --kiss-host 127.0.0.1 \
  --kiss-port 8001
```
The included shell script ```start-serial-kiss.sh``` starts socat configured for local use.
