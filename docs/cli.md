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
| `--hw PRESET` | required | `loraham`, `uputronics-ce0`, `uputronics-ce1`, `waveshare-sx1262`, `meshstick-1262` | `loraham` |
| `--tx-mode MODE` | required | `direct`, `managed` | `managed` |
| `--cad-monitor VAL` | required | `on`, `off` | `off` |
| `--cad-rssi DBM` | required | integer dBm, `-130`..`0` | `-90` |
| `--high-power` | none | — | off |
| `--ocp MA` | required | integer mA the chip can represent (see below) | SX127x `100`, SX1262 `140` |
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

`--high-power` is a bare flag like `--debug`: present means on, absent means off, and any value
(`--high-power=off`) is a usage error. What it permits is in [Boot overrides](#boot-overrides-for-tx-and-cad).

## Radio selection

One process drives one band. The selection is unset until `--radio` is parsed, and the daemon
refuses to start without it:

```
missing option: --radio (433 or 868)
```

The value must be `433` or `868`; anything else is rejected with `invalid radio mode: <value>`
and `allowed: 433, 868`. Both failures print the usage text and exit non-zero. Argument parsing
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
afterwards at runtime. The fourth, `--high-power`, is different in kind: it is a permission, fixed
for the life of the process, that no `CONF` command can grant or revoke.

| Option | Sets | Notes |
| --- | --- | --- |
| `--tx-mode MODE` | the boot TX mode | `managed` when the option is absent |
| `--cad-monitor VAL` | the boot `CAD=0/1` monitoring opt-in | off when absent |
| `--cad-rssi DBM` | the CAD busy RSSI threshold | keeps the built-in `-90` dBm when absent |
| `--high-power` | permission for `POWER=20` on an SX127x board | off when absent; accepted and inert on an SX1262 |
| `--ocp MA` | the PA over-current limit, at every power level | SX127x 100 mA, SX1262 140 mA when absent |

`--cad-monitor` exists so that a `CONF` client which cannot send `SET CADMONITOR=1` still gets the
`CAD=0`/`CAD=1` stream. It takes a required argument: a bare `--cad-monitor` with no value is a
usage error and exits non-zero.

`--cad-rssi` takes a plain integer. Non-integer text and values outside `-130`..`0` are rejected.
What the threshold does at runtime is in [limits.md](limits.md).

`--high-power` permits exactly `POWER=20` on an SX127x board — the datasheet's restricted +20 dBm
mode — and nothing else. **The permission is process-wide:** once the process was started with it,
*any* client's `SET POWER=20` on that band is admitted — a MeshCom firmware or a chat client that
carries 20 in its own configuration will then drive the chip at +20 dBm, and the duty-cycle contract
below applies to everything that band transmits. Otherwise it selects no power (the boot default is unchanged, a client still has to
`SET POWER=20`), it does not admit 18 or 19, and it enforces no duty cycle. With the flag on an
SX127x process the daemon prints a five-line `WARNING` block once at startup and one line per
accepted `POWER=20`:

```
[433] WARNING SX127x +20 dBm permission enabled (--high-power); this flag does not select power
[433] WARNING at +20 dBm: transmit duty cycle <= 1 %, antenna VSWR <= 3:1, chip VDD 2.4-3.7 V (SX1276/7/8 datasheet 5.4.3)
[433] WARNING provide proper cooling for the chip; cooling does not relax these limits
[433] WARNING no duty-cycle measurement or enforcement exists; the operator is responsible
[433] WARNING outside-spec operation can damage hardware; warranty void if disregarded
```

Without the flag a `SET POWER=20` is refused before any hardware effect, with
`high-power mode not enabled (start with --high-power)` as the logged reason (the wire reply is the
ordinary `ERR INVALID`). The permission and the running chip family are reported in `GET STATUS`
as `HIGHPOWER=` and `CHIPFAMILY=` ([conf-protocol.md](conf-protocol.md)). The contract, the
over-current limit and the board caveat are in [limits.md](limits.md) and [hardware.md](hardware.md).

`--ocp MA` sets the PA over-current limit for the life of the process, at every power level,
`POWER=20` included. Only values the chip of the `--hw` preset can represent are accepted, nothing
is rounded: SX127x `0` (protection off), 45–120 in steps of 5, 130–240 in steps of 10; SX1262 5–140
in steps of 5 (the SX1262 has no "off"). It is reported in `GET STATUS` as `OCP=`. One line is
printed at startup when the value is not the default, or when `--high-power` is given on an SX127x
without it:

```
[433] WARNING OCP disabled (--ocp 0): warranty void, hardware may be in danger
[433] WARNING OCP 150 mA above the default 100 mA: warranty void, hardware may be in danger
[868] NOTE OCP 100 mA below the default 140 mA: may limit output power
[433] WARNING OCP not specified under --high-power: the 100 mA default may limit +20 dBm
```

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

`--hw meshstick-1262` refuses to start without its library. It prints
`[CH341] refused: <reason> (exit 4).` without the usage text, and exits `4`. The reasons and the
library are described in [hardware.md](hardware.md#the-meshstick-library-optional).

| Cause | Message |
| --- | --- |
| Bad `--radio` value | `invalid radio mode: <value>` + `allowed: 433, 868` |
| Bad `--tx-mode` value | `invalid TX mode: <value>` + `allowed: direct, managed` |
| Bad `--cad-monitor` value | `invalid CAD monitor value: <value>` + `allowed: on, off` |
| Bad `--cad-rssi` value | `invalid CAD RSSI value: <value>` + `allowed: integer dBm between -130 and 0` |
| Bad `--hw` value (or `meshstick-1262` with `--radio 433`) | `invalid hardware profile: <value>` + `known: loraham, uputronics-ce0, uputronics-ce1, waveshare-sx1262, meshstick-1262` |
| `--high-power` with a value | getopt's `option '--high-power' doesn't allow an argument`, then the usage text |
| `--ocp` not a number | `invalid --ocp (an integer in mA): <value>` |
| `--ocp` the chip cannot represent | `invalid --ocp for <family>: <n> mA (valid: <set>)` |
| Any non-option argument | `Unbekanntes Argument: <arg>` |
| `--radio` missing | `missing option: --radio (433 or 868)` |

## Options removed in version 112

The single-process dual-band mode is gone: `--radio both` is no longer accepted, only `433` and
`868`. The band-suffixed boot options were removed with it. Each maps to the plain option in the
process that owns that band:

| Removed | Replacement |
| --- | --- |
| `--tx-mode-433`, `--tx-mode-868` | `--tx-mode` |
| `--cad-monitor-433`, `--cad-monitor-868` | `--cad-monitor` |
| `--cad-rssi-433`, `--cad-rssi-868` | `--cad-rssi` |
