#!/usr/bin/env bash
# The chat client's own tests: the helpers with the chat source compiled in
# (LORACHAT_NO_MAIN), then the built binary as a process.
set -euo pipefail
cd "$(dirname "$0")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
cc -std=gnu11 -Wall -o "$out/test_socket_options" tests/test_socket_options.c -lncurses -lpthread
"$out/test_socket_options"
# The built binary as its own process: refusals without a terminal, a real
# run on a pseudo-terminal against two listeners.
cc -std=gnu11 -Wall -o "$out/loraham_chat" lorachat_ncurses_113.c -lncurses -lpthread
cc -std=gnu11 -Wall -o "$out/test_chat_binary" tests/test_chat_binary.c -lutil
"$out/test_chat_binary" "$out/loraham_chat"
