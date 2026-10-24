# The test runner's liveness predicate, sourced by run_tests.sh and by
# tests/test_daemon_liveness.sh.
#
# pgrep -x also lists a loraham_daemon in state Z: it has exited and only waits
# for its parent to reap it. A zombie holds no socket, no device and no lock,
# so it is not a running daemon; counting it failed a clean test binary with
# "daemon-still-running" whenever the reap came late.

# The pids of every loraham_daemon that is still alive, one per line.
live_daemon_pids() {
  local pid stat
  for pid in $(pgrep -x loraham_daemon || true); do
    # /proc/<pid>/stat is "pid (comm) state ...": the state follows the last ")".
    # A pid that is already gone is not alive either.
    stat="$(cat "/proc/$pid/stat" 2>/dev/null)" || continue
    stat="${stat##*) }"
    [[ "${stat%% *}" == Z ]] || echo "$pid"
  done
}

daemon_running() {
  [[ -n "$(live_daemon_pids)" ]]
}
