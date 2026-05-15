#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

CC="${CC:-gcc}"
CFLAGS="-Wall -Wextra -std=c11 -I loraham_kiss_tnc ${CFLAGS_EXTRA:-}"
LDFLAGS="${LDFLAGS:-} ${LDFLAGS_EXTRA:-}"

OUT="${1:-loraham_kiss_tnc/loraham_kiss_tnc}"

$CC $CFLAGS \
  -o "$OUT" \
  loraham_kiss_tnc/main.c \
  loraham_kiss_tnc/bridge.c \
  loraham_kiss_tnc/cli.c \
  loraham_kiss_tnc/tcp_server.c \
  loraham_kiss_tnc/kiss.c \
  loraham_kiss_tnc/ax25.c \
  loraham_kiss_tnc/tnc2.c \
  loraham_kiss_tnc/loraham_sock.c \
  loraham_kiss_tnc/loraham_kiss_tnc.c \
  loraham_kiss_tnc/config.c \
  $LDFLAGS

echo "[OK] Built $OUT"
