# Changelog

## 0.10.0

- RF log: `--rflog on|off --rflog-path <absolute>` appends one line per RF frame the
  radio received (with RSSI/SNR) or transmitted (with outcome, after `transmit()`
  returned OK — a CAD refusal or radio error never radiated and is not logged).
  Raw payload as hex and ASCII. Capped at 5 MB by copy-truncate to `<path>.1`; the
  inode never changes, so an external truncate is tolerated. `on` without a path,
  or an unopenable path, refuses to start rather than silently not logging.
- `docs/data-protocol.md`: framed RX frames carry RSSI/SNR in their header; the
  "payload only" sentence was stale.

## 0.9.0

**This entry is where the version scheme changes.** Everything before it was a plain
counter, and `112` was its last value; from here the daemon uses semantic versions.
Nothing about its behaviour changes with this entry.

- Repository restructured: the reference documentation moved out of the daemon's
  README into `docs/`, one subject per page; the client programs into `clients/`;
  the single-file daemons into `archive/`. `loraham_daemon/README.md` is now what
  the daemon is and how to build and run it.
- Every factual claim in the old manual was checked against the source. The
  documentation now describes what the code does -- socket ownership and modes,
  exit codes including the restartable runtime SPI failure, the raw DATA stream
  contract, the SX1262 parameter rasters, and the `--hw` presets.
- `test_multi_instance` no longer reports SKIP when a daemon fails to start on a
  machine that has radio hardware; it checks for the hardware first and fails
  otherwise, like the other live-daemon tests.
- CI runs each ref in its own concurrency group.

## loraham_daemon 112

Breaking:
- `--radio 433|868` is now mandatory (no `--radio both` / implicit default); dual-band runs two processes.
- Band-suffixed flags removed (`--tx-mode-433/868`, `--cad-monitor-433/868`, `--cad-rssi-433/868`); the plain flags apply to the selected band.
- CONF now sends one reply per command: `OK` or `ERR MALFORMED|UNKNOWN|INVALID|BUSY|RADIO_NOT_READY|HARDWARE` (`GET STATUS/STATS/CHANNEL` keep their data line, no trailing `OK`). Parsers must consume it.
- Public sockets moved to `/run/loraham` under systemd; direct/user runs use `LORAHAM_SOCKET_DIR=/tmp`. Bundled clients auto-detect both.
- Daemon runs unprivileged as the `loraham` user (hardware via spi/gpio groups); `GET STATUS` gains `RXREADY`, `GET STATS` gains `RXREARMFAIL`.

Behavior:
- `SET FREQ` is rejected off the selected band (433: 430–440 MHz, 868: 863–870); slow configs are rejected by a 20 s worst-case airtime limit.
- `SET TXQUEUE=0` and radio-touching `SET`s are rejected (`ERR BUSY`) while the TX queue is draining.
- `SET MODE` re-applies the band's boot RF defaults; boot fails closed if any RF setter or RX-arm fails; a persistently deaf receiver escalates to `RADIO=FAILED`.
- MANAGED TX aborts on a failed/unavailable CAD probe instead of treating it as free; a pending RX packet is no longer destroyed by a CAD probe or a rejected CONF line.
- Per-process GPIO/SPI locks fail the boot closed on conflict; startup lock failures exit 4 (no restart), runtime SPI fatals exit 5 (restartable), duplicate instance exits 3.
- SX1262: all `OOK`, `ENCODING=1`, and `FREQDEV<0.6` rejected at prevalidation; RF-switch failure now fails closed.

New hardware:
- Uputronics Raspberry Pi Zero LoRa(TM) Expansion Board V2.5C.
- Waveshare SX1262 LoRaWAN/GNSS HAT.

---

Entries for `111a` and earlier are in the git history of this file.
