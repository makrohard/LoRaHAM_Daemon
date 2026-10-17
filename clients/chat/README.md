# LoRaHAM Chat

An ncurses chat and APRS TUI on the 433 DATA and CONF sockets. It needs a real terminal; there is
no headless mode.

Settings live in `lorachat.conf` in the working directory — callsign, TX/RX frequency, APRS
destination and path. The in-app `Ctrl-K` menu edits them and writes back to that file.

    sudo apt install libncurses5-dev libncursesw5-dev -y
    gcc clients/chat/lorachat_ncurses_113.c -o loraham_chat -lncurses -lpthread

    ./loraham_chat

It finds the sockets under `/run/loraham` first and falls back to `/tmp`, so it works both under
the systemd deployment and beside a daemon started with `LORAHAM_SOCKET_DIR=/tmp`.

For a daemon started with `--id`, give its sockets (chat uses the raw data socket and the CONF
socket, not the framed one); a path given here is used as is:

    ./loraham_chat --data-socket /run/loraham/lora-ms1.sock --conf-socket /run/loraham/loraconf-ms1.sock

`-h` prints the usage. An empty or over-long path, a path that cannot be connected or an unknown
argument stops chat with the reason before the screen starts. Chat tunes the radio from
`lorachat.conf`: for an 868 daemon set an 868 frequency there, or the daemon refuses the 433 default.
The tests run without a terminal: `clients/chat/run_tests.sh`.

### Changes

- v1.3.0: `--data-socket` and `--conf-socket`, so chat follows a daemon started with `--id`. Without
  them chat behaves as before.

Chat and the iGate cannot sensibly run at the same time — see
[the note in the client index](../README.md#running-chat-and-the-igate-at-the-same-time).

## Start a daemon first

Chat needs a 433 daemon; it uses that band's DATA and CONF sockets.

```bash
mkdir -p ~/loraham-run && chmod 755 ~/loraham-run
LORAHAM_SOCKET_DIR=/tmp LORAHAM_RUNTIME_DIR=~/loraham-run \
  ./loraham_daemon --radio 433 --hw loraham
```

`--hw` names your board and `LORAHAM_RUNTIME_DIR` is the lock directory, which must not be
group-writable — [why](../README.md#start-a-daemon-first). Chat runs happily under the default
`managed` transmit mode, which is what LoRaHAM_Pi Control uses for it.

## Licence

    Copyright (c) 2020-2026 Alexander Walter
    Licensed under the GNU GPL v3 (see ../../LICENSE)

The licence header is also at the top of the source file.
