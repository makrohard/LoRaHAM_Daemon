# Command line

Every option `loraham_daemon` accepts, what each one defaults to, and the RF settings the selected
band starts with. Board wiring and the meaning of the `--hw` presets live in
[hardware.md](hardware.md); unit files, socket paths and running two bands at once live in
[deployment.md](deployment.md); changing RF settings on a running daemon is
[conf-protocol.md](conf-protocol.md).

## Contents

- [Synopsis](#synopsis)
- [Options](#options)
- [Radio selection](#radio-selection)
- [Foreground and background](#foreground-and-background)
- [Boot overrides for TX and CAD](#boot-overrides-for-tx-and-cad)
- [Startup RF defaults](#startup-rf-defaults)
- [Argument errors](#argument-errors)
- [Options removed in version 112](#options-removed-in-version-112)

## Synopsis

```
loraham_daemon --radio 433|868 [options]
```

`--radio` is the only mandatory option. With nothing else given, the daemon runs in the
foreground and prints traffic to the terminal; debug lines appear only with `--debug`.

```
./loraham_daemon --radio 433
./loraham_daemon --radio 868
```

The first invocation owns the 433 MHz radio, the second the 868 MHz radio.

## Options

| Option | Argument | Accepted values | Default |
| --- | --- | --- | --- |
| `-d`, `--daemon` | none | — | off (foreground) |
| `-v`, `--version` | none | — | — |
| `--debug` | none | — | off |
| `--radio MODE` | required | `433`, `868` | none — must be given |
| `--hw PRESET` | required | `loraham`, `uputronics-ce0`, `uputronics-ce1`, `waveshare-sx1262` | `loraham` |
| `--tx-mode MODE` | required | `direct`, `managed` | `managed` |
| `--cad-monitor VAL` | required | `on`, `off` | `off` |
| `--cad-rssi DBM` | required | integer dBm, `-130`..`0` | `-90` |
| `-h`, `--help` | none | — | — |

`-v` / `--version` prints the version and exits with success. `-h` / `--help` prints the usage
text — options, defaults and the socket paths of both bands — and exits with success. The version
string comes from `daemon_version.h` alone, which is also what the daemon prints at startup:

```
$ ./loraham_daemon --version
loraham_daemon 0.9.0

$ ./loraham_daemon --radio 433
[Daemon] loraham_daemon 0.9.0
```

`--tx-mode` and `--cad-monitor` compare case-insensitively. `--radio` and `--hw` match exactly, so
`--hw LORAHAM` is rejected with the list of known presets. What each `--hw` preset wires up is in [hardware.md](hardware.md).

`--cad-rssi` is a supported boot option and is listed by `--help`, though it went undocumented for
a long time.

## Radio selection

One process drives one band. The selection is unset until `--radio` is parsed, and the daemon
refuses to start without it:

```
Fehlende Option: --radio (433 oder 868)
```

The value must be `433` or `868`; anything else is rejected with `Ungültiger Radio-Modus: <wert>`
and `Erlaubt: 433, 868`. Both failures print the usage text and exit non-zero. Argument parsing
runs before any I/O setup, so a missing or invalid selection fails before locks, GPIO, sockets or
RadioLib are touched.

Only the sockets of the selected band are created. Running both bands means running two processes;
see [deployment.md](deployment.md).

## Foreground and background

Without `-d` the daemon stays in the foreground and writes to the terminal.

`-d` / `--daemon` puts it into the background: double `fork()` with `setsid()` between them,
`umask(007)`, `chdir("/")`, `stdin` from `/dev/null`, and `stdout` plus `stderr` redirected to the
single file `/tmp/lora_daemon.log`. That file is not per band — two backgrounded processes share
it. Under a supervisor such as systemd the flag is unnecessary: the service manager already
detaches the process and captures its output.

`--debug` raises the log level from the default normal level to debug. Without it, debug lines are
suppressed in both foreground and background mode.

## Boot overrides for TX and CAD

The three boot options below set the starting state of the one band selected by `--radio`. Each is
applied to the radio controller after I/O init, and `CONF` can still change the same settings
afterwards at runtime.

| Option | Sets | Notes |
| --- | --- | --- |
| `--tx-mode MODE` | the boot TX mode | `managed` when the option is absent |
| `--cad-monitor VAL` | the boot `CAD=0/1` monitoring opt-in | off when absent |
| `--cad-rssi DBM` | the CAD busy RSSI threshold | keeps the built-in `-90` dBm when absent |

`--cad-monitor` exists so that a `CONF` client which cannot send `SET CADMONITOR=1` still gets the
`CAD=0`/`CAD=1` stream. It takes a required argument: a bare `--cad-monitor` with no value is a
usage error and exits non-zero.

`--cad-rssi` takes a plain integer. Non-integer text and values outside `-130`..`0` are rejected.
What the threshold does at runtime is in [limits.md](limits.md).

## Startup RF defaults

Both bands come up in LoRa mode. These values are applied inside the driver during startup, before
any `CONF` client connects.

| Setting | `--radio 433` | `--radio 868` |
| --- | --- | --- |
| Mode | `LORA` | `LORA` |
| `FREQ` | `433.900` MHz | `869.525` MHz |
| `SF` | `12` | `11` |
| `BW` | `125` kHz | `250` kHz |
| `CR` | `5` | `5` |
| `CRC` | on | on |
| `PREAMBLE` | `8` | `16` |
| `SYNC` | `0x12` | `0x2B` |
| `LDRO` | forced `1` | `AUTO` |
| `POWER` | `10` dBm | `10` dBm |

The 433 band calls `autoLDRO()` and then `forceLDRO(1)`; the 868 band calls `autoLDRO()` only, so
the chip decides. The parameter names and their runtime ranges are in
[conf-protocol.md](conf-protocol.md).

## Argument errors

Every rejected option prints a reason, then the usage text, then exits non-zero.

| Cause | Message |
| --- | --- |
| Bad `--radio` value | `Ungültiger Radio-Modus: <wert>` + `Erlaubt: 433, 868` |
| Bad `--tx-mode` value | `Ungültiger TX-Modus: <wert>` + `Erlaubt: direct, managed` |
| Bad `--cad-monitor` value | `Ungültiger CAD-Monitor-Wert: <wert>` + `Erlaubt: on, off` |
| Bad `--cad-rssi` value | `Ungültiger CAD-RSSI-Wert: <wert>` + `Erlaubt: Ganzzahl dBm zwischen -130 und 0` |
| Bad `--hw` value | `Ungültiges Hardware-Profil: <wert>` + `Bekannt: loraham, uputronics-ce0, uputronics-ce1, waveshare-sx1262` |
| Any non-option argument | `Unbekanntes Argument: <arg>` |
| `--radio` missing | `Fehlende Option: --radio (433 oder 868)` |

## Options removed in version 112

The single-process dual-band mode is gone: `--radio both` is no longer accepted, only `433` and
`868`. The band-suffixed boot options were removed with it. Each maps to the plain option in the
process that owns that band:

| Removed | Replacement |
| --- | --- |
| `--tx-mode-433`, `--tx-mode-868` | `--tx-mode` |
| `--cad-monitor-433`, `--cad-monitor-868` | `--cad-monitor` |
| `--cad-rssi-433`, `--cad-rssi-868` | `--cad-rssi` |
