# LoRaHAM KISS TNC Bridge
written by Johannes Loose 410733@gmail.com

KISS/TCP TNC bridge for `LoRaHAM_Daemon`.

It exposes a KISS/TCP port for APRS clients and talks to the LoRaHAM daemon through its framed DATA Unix socket.

```text
APRS client <-> KISS/TCP <-> loraham_kiss_tnc <-> /tmp/lora433f.sock <-> loraham_daemon
```
## Limitations
- single KISS/TCP client
- KISS port 0 only
- APRS/TNC2 text payload only
- TX path intentionally blocks during tx timing
- mode=LORA only - because FSK is not used in LoRa APRS
- framed DATA socket only for daemon packet I/O
- queues TX packets and uses CONF events (`TX=`, `CAD=`, `STATUS`)

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
      --cad-wait-ms MS     Max polite wait for busy channel
      --cad-idle-ms MS     Required CAD idle stability
      --cad-ignore         Ignore CAD/channel busy state
      --tx-queue-len N     Queued TX packet limit
      --tx-packet-ttl-ms MS
                            Max queued packet lifetime
  -v, --verbose            Verbose output
      --version            Print version and exit
  -h, --help               Show help

```

## Serial KISS

Xastir ans YAAC only support Serial KISS TNC. You can use socat to forward the byte-stream to a PTY.
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