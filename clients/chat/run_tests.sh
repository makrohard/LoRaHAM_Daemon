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
# Test-only: the same binary test under ThreadSanitizer (one drawing thread;
# the receive thread shares only the locked line queue). Required in CI; on a
# machine without the TSan runtime it is skipped, and says so.
if echo 'int main(void){return 0;}' | cc -x c -fsanitize=thread -o "$out/tsan_probe" - 2>/dev/null; then
  cc -std=gnu11 -Wall -g -fsanitize=thread -o "$out/loraham_chat_tsan" lorachat_ncurses_113.c -lncurses -lpthread
  TSAN_OPTIONS="log_path=$out/tsan" "$out/test_chat_binary" "$out/loraham_chat_tsan"
  if ls "$out"/tsan.* >/dev/null 2>&1; then
    cat "$out"/tsan.*
    echo "[FAIL] ThreadSanitizer report(s) from the chat binary"
    exit 1
  fi
  echo "[ OK ] no ThreadSanitizer report from the chat binary"
elif [ -n "${CI:-}" ]; then
  echo "[FAIL] no ThreadSanitizer runtime in CI: the TSan step cannot run"
  exit 1
else
  echo "[SKIP] no ThreadSanitizer runtime here: the TSan step did not run"
fi
