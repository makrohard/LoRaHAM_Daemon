# The test runner's liveness predicate, sourced by run_tests.sh and by
# tests/test_daemon_liveness.sh.
#
# pgrep -x also lists a loraham_daemon in state Z: it has exited and only waits
# for its parent to reap it. A zombie holds no socket, no device and no lock,
# so it is not a running daemon; counting it failed a clean test binary with
# "daemon-still-running" whenever the reap came late.
#
# Only what is observed is excluded: a pid in state Z, or a pid whose /proc
# entry is gone. Anything the predicate cannot read counts as a live daemon, so
# a check never passes on a read it could not make. One exception, with
# LIVENESS_SCOPE set: a daemon whose executable cannot be read counts as
# foreign (not the caller's, never stopped), since the caller can always read
# the executables of the processes it started (in_scope below).

# Scope (1.5.0): LIVENESS_SCOPE, when set, is a ':'-separated list of
# directories; only a loraham_daemon whose executable lies under one of them
# is the caller's, so a lab or system daemon running next to the caller is
# neither counted nor stopped. run_tests.sh sets it to its own tree and its
# temp directory (the tests run the daemon from there). Unset: every
# loraham_daemon counts. An executable that cannot be read is not the
# caller's: the caller can always read the processes it started.
in_scope() {     # <pid>
  [[ -z "${LIVENESS_SCOPE:-}" ]] && return 0
  local exe dir dirs
  exe="$(readlink "/proc/$1/exe" 2>/dev/null)" || return 1
  IFS=: read -r -a dirs <<< "$LIVENESS_SCOPE"
  for dir in "${dirs[@]}"; do
    [[ -n "$dir" && "$exe" == "${dir%/}"/* ]] && return 0
  done
  return 1
}

# The pids of every loraham_daemon in scope that may still be alive, one per
# line. Returns 2 when the process table cannot be listed (pgrep failed for
# another reason than "no match").
live_daemon_pids() {
  local pids pid stat rc=0
  pids="$(pgrep -x loraham_daemon)" || rc=$?
  case "$rc" in
    0) ;;
    1) return 0 ;;
    *) echo "ERROR: pgrep -x loraham_daemon failed (rc $rc)" >&2; return 2 ;;
  esac
  for pid in $pids; do
    # /proc/<pid>/stat is "pid (comm) state ...": the state follows the last ")".
    if stat="$(cat "/proc/$pid/stat" 2>/dev/null)"; then
      stat="${stat##*) }"
      [[ "${stat%% *}" == Z ]] && continue
    elif [[ ! -e "/proc/$pid" ]]; then
      continue
    fi
    in_scope "$pid" || continue
    echo "$pid"
  done
}

# "<pid> <executable>" for every live daemon in scope (for the error output).
list_daemons() {
  local pid
  for pid in $(live_daemon_pids); do
    echo "$pid $(readlink "/proc/$pid/exe" 2>/dev/null || echo '?')"
  done
}

# Send <signal> to every live daemon in scope, and to nothing else.
stop_daemons() { # <signal>
  local pids
  pids="$(live_daemon_pids)" || return 0
  [[ -z "$pids" ]] && return 0
  # shellcheck disable=SC2086
  kill "-$1" $pids 2>/dev/null || true
}

# A process table that cannot be listed counts as a daemon running.
daemon_running() {
  local pids
  pids="$(live_daemon_pids)" || return 0
  [[ -n "$pids" ]]
}
