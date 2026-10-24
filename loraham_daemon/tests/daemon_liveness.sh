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
# a check never passes on a read it could not make.

# The pids of every loraham_daemon that may still be alive, one per line.
# Returns 2 when the process table cannot be listed (pgrep failed for another
# reason than "no match").
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
    echo "$pid"
  done
}

# A process table that cannot be listed counts as a daemon running.
daemon_running() {
  local pids
  pids="$(live_daemon_pids)" || return 0
  [[ -n "$pids" ]]
}
