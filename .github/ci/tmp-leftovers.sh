#!/usr/bin/env bash
# The CI check "No test files left in /tmp": tmp-leftovers.sh BEFORE AFTER DIR lists DIR (sorted, into
# AFTER), compares it with BEFORE (the same listing taken before the tests) and fails (exit 1) with every
# new entry, except one kind that is no test's: systemd-private-* owned by root (uid TMP_SYSTEMD_UID,
# default 0), systemd's PrivateTmp of a service the runner's OS started meanwhile (seen: timedated).
# A failed listing fails the check.
set -euo pipefail
before="$1" after="$2" dir="$3" systemd_uid="${TMP_SYSTEMD_UID:-0}"
ls -A "$dir" | sort > "$after"
left=()
while IFS= read -r name; do
  if [[ "$name" == systemd-private-* && "$(stat -c %u -- "$dir/$name")" == "$systemd_uid" ]]; then
    echo "ignored, systemd's (uid $systemd_uid): $name"
  else
    left+=("$name")
  fi
done < <(comm -13 "$before" "$after")
if (( ${#left[@]} > 0 )); then
  echo "FAIL: the test run left these new entries in $dir:"
  printf '%s\n' "${left[@]}"
  exit 1
fi
echo "OK: no new entry in $dir"
