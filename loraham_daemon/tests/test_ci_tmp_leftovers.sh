#!/usr/bin/env bash
# The CI /tmp check (.github/ci/tmp-leftovers.sh) against a directory of its own: a systemd-private-*
# entry owned by the systemd uid is ignored; the same entry owned by anyone else, any other new entry,
# and a name that only contains "systemd-private-" still fail. The test cannot create a root-owned entry,
# so it names its own uid as the systemd uid (TMP_SYSTEMD_UID) for the ignored rows.
set -euo pipefail
check="$(cd "$(dirname "$0")/../.." && pwd)/.github/ci/tmp-leftovers.sh"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
me="$(id -u)" other="$(( $(id -u) + 1 ))"
fails=0
row() {   # row NAME EXPECTED_RC UID ENTRY...: the check over DIR with ENTRY... new since the listing
  local name="$1" want="$2" uid="$3"; shift 3
  rm -rf -- "$work/dir"; mkdir "$work/dir"
  ls -A "$work/dir" | sort > "$work/before"
  local e; for e in "$@"; do mkdir "$work/dir/$e"; done
  local rc=0
  TMP_SYSTEMD_UID="$uid" "$check" "$work/before" "$work/after" "$work/dir" > "$work/out" 2>&1 || rc=$?
  if [[ "$rc" == "$want" ]]; then echo "[ OK ] $name"; else echo "[FAIL] $name: rc $rc, want $want"; cat "$work/out"; fails=$((fails + 1)); fi
}
row "nothing new: ok" 0 "$me"
row "systemd-private-* of the systemd uid: ignored" 0 "$me" systemd-private-abc-systemd-timedated.service-x
row "systemd-private-* of another uid: fails" 1 "$other" systemd-private-abc-systemd-timedated.service-x
row "another leftover next to an ignored one: fails" 1 "$me" systemd-private-abc loraham-test-left
if grep -qx "loraham-test-left" "$work/out" && ! grep -qx "systemd-private-abc" "$work/out"; then
  echo "[ OK ] only the other leftover is listed"
else
  echo "[FAIL] the listing names the wrong entries"; cat "$work/out"; fails=$((fails + 1))
fi
row "a name only containing systemd-private-: fails" 1 "$me" xsystemd-private-abc
rm -rf -- "$work/dir"
if TMP_SYSTEMD_UID="$me" "$check" "$work/before" "$work/after" "$work/dir" > "$work/out" 2>&1; then
  echo "[FAIL] a failed listing passed"; fails=$((fails + 1))
else
  echo "[ OK ] a failed listing fails"
fi
echo "Summary: fail=$fails"
exit $(( fails > 0 ))
