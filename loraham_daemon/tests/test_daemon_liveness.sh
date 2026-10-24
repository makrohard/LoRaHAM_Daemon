#!/usr/bin/env bash
# The runner's liveness predicate (tests/daemon_liveness.sh) against real processes:
# - a loraham_daemon that exited and awaits its reap (state Z) is not running;
# - a live loraham_daemon still is, also next to a zombie.
#
# pgrep -x matches the kernel's process name, which is the executed file's
# basename, so each fixture execs a real file named loraham_daemon. A zombie
# lasts while its parent lives without reaping it: the parent execs `cat` on a
# FIFO this test owns and blocks there until the test writes to it. Validity is
# asserted from /proc (parent alive, child in state Z), never assumed from time.
#
# Every wait observes a state (a name in /proc/<pid>/comm, a state letter, a
# /proc entry gone) and is bounded; none is a fixed sleep. The error paths
# (pgrep failing, /proc unreadable) are checked with pgrep and cat shadowed by
# shell functions, so they are deterministic.
#
# Run it on its own (CI does, in its own step): any other loraham_daemon alive
# meanwhile would also be counted.
set -euo pipefail
cd "$(dirname "$0")"
# shellcheck source=daemon_liveness.sh
. ./daemon_liveness.sh

tmp="$(mktemp -d)"
parents=()
cleanup() {      # only reached with fixtures left on a failure path: end them, never block on a FIFO
  local p
  for p in "${parents[@]}"; do kill "$p" 2>/dev/null || true; done
  pkill -x -P "$$" loraham_daemon 2>/dev/null || true
  for p in "${parents[@]}"; do wait "$p" 2>/dev/null || true; done
  rm -rf "$tmp"
}
trap cleanup EXIT

fails=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
state_of() { local s; s="$(cat "/proc/$1/stat" 2>/dev/null)" || return 1; s="${s##*) }"; echo "${s%% *}"; }

# Wait for an observed state, bounded; the caller asserts what it found.
await_state() {    # <pid> <want: Z | alive | gone>
  local i s
  for i in $(seq 1 500); do
    s="$(state_of "$1")" || s=gone
    case "$2:$s" in Z:Z) return 0;; alive:[RSD]) return 0;; gone:gone) return 0;; esac
    sleep 0.01
  done
  return 1
}

# Wait, bounded, until the process runs the expected program (its exec is done).
await_comm() {     # <pid> <name>
  local i c
  for i in $(seq 1 500); do
    c=""; read -r c < "/proc/$1/comm" 2>/dev/null || true
    [[ "$c" == "$2" ]] && return 0
    sleep 0.01
  done
  return 1
}

# pgrep's own answer: rc 1 is "none", anything above 1 is an error.
no_daemon_listed() {
  local rc=0
  pgrep -x loraham_daemon > /dev/null || rc=$?
  case "$rc" in 1) return 0;; 0) return 1;; *) echo "FAIL: pgrep failed (rc $rc)"; exit 1;; esac
}

no_daemon_listed || { echo "FAIL: a loraham_daemon is already running; run this test on its own"; exit 1; }

mkdir "$tmp/zombie" "$tmp/live"
cp "$(command -v cat)" "$tmp/zombie/loraham_daemon"
cp "$(command -v cat)" "$tmp/live/loraham_daemon"

# A zombie loraham_daemon. The child (a cat named loraham_daemon) blocks on the
# "go" FIFO; the parent shell execs cat on the "hold" FIFO. Only once /proc shows
# the parent is that cat (which never reaps) is the child let go, so its exit
# leaves a zombie that no shell's SIGCHLD handling can reap first.
mkfifo "$tmp/go-zombie" "$tmp/hold-zombie"
sh -c '"$1" "$2" > /dev/null & exec cat "$3" > /dev/null' sh "$tmp/zombie/loraham_daemon" "$tmp/go-zombie" "$tmp/hold-zombie" &
zparent=$!; parents+=("$zparent")
zchild=""
for _ in $(seq 1 500); do
  zchild="$(pgrep -x -P "$zparent" loraham_daemon || true)"
  [[ -n "$zchild" && "$(cat "/proc/$zparent/comm" 2>/dev/null)" == cat ]] && break
  sleep 0.01
done
if [[ -z "$zchild" ]]; then fail "the zombie fixture: pgrep -x does not see the child"; exit 1; fi
[[ "$(cat "/proc/$zparent/comm" 2>/dev/null)" == cat ]] || { fail "the zombie fixture: the parent did not become cat"; exit 1; }
: > "$tmp/go-zombie"
await_state "$zchild" Z || { fail "the zombie fixture: child $zchild not in state Z ($(state_of "$zchild" || echo gone))"; exit 1; }
await_state "$zparent" alive || { fail "the zombie fixture: parent $zparent not alive"; exit 1; }
pass "fixture: pgrep -x sees $zchild, state Z; its parent $zparent is alive and has not reaped it"

if daemon_running; then fail "a zombie loraham_daemon counts as running"; else pass "a zombie loraham_daemon is not running"; fi

# A live loraham_daemon (a cat blocked on its own FIFO), next to the zombie.
mkfifo "$tmp/hold-live"
"$tmp/live/loraham_daemon" "$tmp/hold-live" > /dev/null &
lpid=$!; parents+=("$lpid")
await_comm "$lpid" loraham_daemon || { fail "the live fixture: $lpid never ran loraham_daemon"; exit 1; }
await_state "$lpid" alive || { fail "the live fixture: $lpid not alive"; exit 1; }
[[ "$(pgrep -x loraham_daemon | sort | tr '\n' ' ')" == "$(printf '%s\n' "$zchild" "$lpid" | sort | tr '\n' ' ')" ]] \
  || { fail "the fixtures: pgrep -x does not list exactly the two"; exit 1; }
await_state "$zchild" Z || { fail "the zombie was reaped before the check"; exit 1; }

if daemon_running; then pass "a live loraham_daemon is running"; else fail "a live loraham_daemon is not counted"; fi
got="$(live_daemon_pids)"
if [[ "$got" == "$lpid" ]]; then pass "live_daemon_pids lists the live one only"; else fail "live_daemon_pids gave '$got', want '$lpid'"; fi

# Release both fixtures; the parents exit. The zombie is then reaped by
# whoever inherits it, so its disappearance is observed, not assumed.
: > "$tmp/hold-zombie"
: > "$tmp/hold-live"
wait "$zparent" "$lpid"
await_state "$zchild" gone || fail "the zombie $zchild was not reaped after its parent exited"
if no_daemon_listed; then pass "released: no loraham_daemon left"; else fail "a loraham_daemon is left after the release"; fi

# The error paths. pids are always below pid_max, so pid_max is never a pid.
nopid="$(cat /proc/sys/kernel/pid_max)"
if ( pgrep() { return 3; }; daemon_running 2> /dev/null ); then
  pass "pgrep failing (rc 3): counted as a daemon running"
else
  fail "pgrep failing (rc 3) reads as no daemon"
fi
rc=0; ( pgrep() { return 3; }; live_daemon_pids > /dev/null 2>&1 ) || rc=$?
if [[ "$rc" -eq 2 ]]; then pass "pgrep failing: live_daemon_pids returns 2"; else fail "pgrep failing: live_daemon_pids returned $rc, want 2"; fi
if ( pgrep() { return 1; }; daemon_running ); then fail "pgrep with no match reads as a daemon"; else pass "pgrep with no match: no daemon"; fi
got="$( pgrep() { echo "$$"; }; cat() { return 1; }; live_daemon_pids )"
if [[ "$got" == "$$" ]]; then pass "an unreadable stat of an existing pid counts as live"; else fail "an unreadable stat of existing pid $$ gave '$got'"; fi
got="$( pgrep() { echo "$nopid"; }; cat() { return 1; }; live_daemon_pids )"
if [[ -z "$got" ]]; then pass "an unreadable stat of a pid that is gone does not count"; else fail "a gone pid $nopid gave '$got'"; fi

if [[ "$fails" -ne 0 ]]; then echo "test_daemon_liveness: $fails FAIL"; exit 1; fi
echo "test_daemon_liveness: OK"
