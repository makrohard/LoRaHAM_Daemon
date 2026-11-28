#!/usr/bin/env bash
# A daemon built without lgpio (build.sh on a host without <lgpio.h>, CI's no-lgpio job), given as $1 with the
# Meshstick library its build made beside it (lib/). Its runs: no lgpio is linked; every SPI preset is refused at
# start (exit 4) before any lock, socket or device; a USB stick preset passes that gate. The runs need no stick: the
# USB rows ask for a serial no stick has, so an attached stick is never opened.
# Positive controls show that the dirs this test watches are the ones the daemon uses: a USB row takes its instance
# lock in LORAHAM_RUNTIME_DIR and binds its sockets in LORAHAM_SOCKET_DIR, and a LORAHAM_SOCKET_DIR too long for a
# socket path is refused by name.
set -euo pipefail
bin="$(realpath -- "$1")"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
USB_PRESETS="meshstick-1262 pinedio-usb-1262 ch341-rfm98pw-433"
NO_STICK=T2NOSTK
fails=0
ok() { echo "[ OK ] $1"; }
bad() { echo "[FAIL] $1"; cat "$work/out" 2>/dev/null || true; fails=$((fails + 1)); }

# [place=NAME] run SOCKDIR BINARY ARGS...: one start with fresh dirs (with place, a regular file NAME in SOCKDIR
# first); rc in $rc, output in $work/out, files left in the two dirs in $work/files. The one place the dirs reach
# the daemon.
run() {
  local sock="$1" b="$2"; shift 2
  rm -rf -- "$work/rt" "$sock"
  mkdir -p -- "$work/rt" "$sock"
  chmod 700 "$work/rt" "$sock"
  [[ -z "${place:-}" ]] || : > "$sock/$place"
  rc=0
  LORAHAM_SOCKET_DIR="$sock" LORAHAM_RUNTIME_DIR="$work/rt" timeout 30 "$b" "$@" > "$work/out" 2>&1 || rc=$?
  find "$work/rt" "$sock" -mindepth 1 > "$work/files"
}

# 1. Linked libraries: the C runtime, no lgpio.
readelf -d "$bin" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' > "$work/needed"
if grep -qx 'libc.so.6' "$work/needed" && ! grep -q lgpio "$work/needed"; then
  ok "NEEDED holds libc.so.6 and no lgpio"
else
  bad "NEEDED: $(tr '\n' ' ' < "$work/needed")"
fi

# 2. Positive control, runtime dir: a USB preset passes the gate, takes its lock there, then finds no such stick.
[[ -e "$(dirname -- "$bin")/lib/libloraham-ch341.so" ]] || bad "precondition: the Meshstick library beside $bin"
run "$work/sock" "$bin" --hw meshstick-1262 --radio 868 --usb-serial "$NO_STICK"
if [[ "$rc" == 4 ]] && grep -qE "^([^ ]+Z )?\[CH341\] refused: no ELECROW Meshstick with serial $NO_STICK \(exit 4\)\.$" "$work/out" &&
   grep -qx "$work/rt/instance-868.lock" "$work/files" && ! grep -qF '[GPIO] refused' "$work/out"; then
  ok "meshstick-1262: passes the gate, lock in LORAHAM_RUNTIME_DIR, no such stick (exit 4)"
else
  bad "meshstick-1262 positive control: rc $rc, files: $(tr '\n' ' ' < "$work/files")"
fi

# 3. Positive control, socket dir: the daemon reads LORAHAM_SOCKET_DIR (a path too long for sun_path is refused).
long="$work/$(printf 's%.0s' {1..120})"
run "$long" "$bin" --hw loraham --radio 868
if [[ "$rc" == 4 ]] && grep -qF "[SOCKET] refused: socket path too long" "$work/out" && grep -qF "$long/" "$work/out"; then
  ok "LORAHAM_SOCKET_DIR is the daemon's socket dir (too long -> refused by name, exit 4)"
else
  bad "socket dir positive control: rc $rc"
fi

# 3b. Positive control, socket dir at the bind step: the USB row binds its sockets after the lock and before it looks
# for the stick, so a regular file where its 868 data socket goes stops it there, by that path.
place=lora868.sock run "$work/sock" "$bin" --hw meshstick-1262 --radio 868 --usb-serial "$NO_STICK"
if [[ "$rc" == 1 ]] && grep -qF "ERROR: socket path exists but is not a socket: $work/sock/lora868.sock" "$work/out" &&
   grep -qF "[Daemon] socket setup 868 failed, exiting." "$work/out" && ! grep -qF '[GPIO] refused' "$work/out"; then
  ok "meshstick-1262: binds its sockets in LORAHAM_SOCKET_DIR (a file at lora868.sock there stops it, exit 1)"
else
  bad "socket dir bind control: rc $rc"
fi

# 4. Every SPI preset the binary knows (its --help list without the USB sticks), on both bands: refused, nothing
# created.
presets="$("$bin" --help 2>&1 | sed -n 's/^ *--hw PRESET  hardware profile: //p' | tr -d ',')"
spi=""
for p in $presets; do [[ " $USB_PRESETS " == *" $p "* ]] || spi="$spi $p"; done
if [[ " $presets " == *" loraham "* && " $spi " == *" loraham "* ]]; then
  ok "SPI presets from --help:$spi"
else
  bad "no SPI preset list from --help: '$presets'"
fi
for p in $spi; do
  for band in 433 868; do
    run "$work/sock" "$bin" --hw "$p" --radio "$band"
    want="[GPIO] refused: preset $p needs the Pi's SPI and GPIO, and this daemon was built without lgpio (USB stick presets only: meshstick-1262, pinedio-usb-1262, ch341-rfm98pw-433) (exit 4)."
    if [[ "$rc" == 4 ]] && grep -qxF -- "$want" "$work/out" && [[ ! -s "$work/files" ]]; then
      ok "$p $band: refused (exit 4), no file"
    else
      bad "$p $band: rc $rc, files: $(tr '\n' ' ' < "$work/files")"
    fi
  done
done

# 5. A USB preset without the library beside the binary: past the gate, refused for the missing library.
mkdir -p -- "$work/bare"
cp -- "$bin" "$work/bare/loraham_daemon"
run "$work/sock" "$work/bare/loraham_daemon" --hw meshstick-1262 --radio 868 --usb-serial "$NO_STICK"
if [[ "$rc" == 4 ]] && grep -qF "[CH341] refused: Meshstick library not found" "$work/out" &&
   ! grep -qF '[GPIO] refused' "$work/out"; then
  ok "meshstick-1262 without lib/: passes the gate, refused for the library (exit 4)"
else
  bad "meshstick-1262 without lib/: rc $rc"
fi

echo "Summary: fail=$fails"
exit $(( fails > 0 ))
