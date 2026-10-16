# shellcheck shell=bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
#
# Sourced by the tests here: counts each case, prints it, and reports.

failures=0
count=0

pass() {
  count=$((count + 1))
  echo "ok   $1"
}

fail() {
  count=$((count + 1))
  failures=$((failures + 1))
  echo "FAIL $1"
}

# expect NAME ACTUAL WANT
expect() {
  if [ "$2" = "$3" ]; then
    pass "$1"
  else
    fail "$1"
    printf '     | got:  %s\n     | want: %s\n' "$2" "$3"
  fi
}

# report: the tally; fails if any case did.
report() {
  echo "$((count - failures)) of $count passed"
  [ "$failures" -eq 0 ]
}
