#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
#
# Stops, deregisters and deletes one repository's self-hosted runners on this
# host, leaving other repositories' runners alone. Use it to decommission or
# migrate a host (README.md).
#
#   bash tools/runners/teardown-runners.sh <recon|gfx>
#
# Run as the user that owns the runner directories. It keeps going past a
# runner it cannot remove, and exits non-zero if any is left.
set -uo pipefail

# shellcheck source-path=SCRIPTDIR source=common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

[ $# -eq 1 ] || {
  echo "usage: $0 <recon|gfx>" >&2
  exit 2
}
select_repo "$1"

find_runner_dirs
if [ "${#RUNNER_DIRS[@]}" -eq 0 ]; then
  echo "No runner of $REPO on this host (looked in $RUNNER_ROOT/ and ~/actions-runner-*/)."
  exit 0
fi

kept=0
for dir in "${RUNNER_DIRS[@]}"; do
  if [ -f "$dir/.runner" ] && ! registered_to_repo "$dir"; then
    echo "==> Skipping $dir: registered to another repository"
    continue
  fi
  echo "==> Removing $dir"
  svc "$dir" stop >/dev/null 2>&1 || true
  svc "$dir" uninstall >/dev/null 2>&1 || true
  # Delete a registered runner's directory only once it is deregistered: it
  # holds the credentials deregistering needs, so deleting it first would
  # strand an offline registration that nothing on this host can remove.
  if [ -f "$dir/.runner" ] && ! (
    cd "$dir" || exit 1
    if ! { command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; }; then
      echo "    gh is not authed: the service is stopped, but the runner is still registered."
      echo "    Run 'gh auth login' and re-run, or remove it under Settings -> Actions -> Runners."
      exit 1
    fi
    token="$(gh api -X POST "repos/$REPO/actions/runners/remove-token" --jq .token)" || token=""
    if [ -z "$token" ]; then
      echo "    could not mint a remove token"
      exit 1
    fi
    ./config.sh remove --token "$token"
  ); then
    echo "    Kept $dir (still registered): fix the cause and re-run, or remove it"
    echo "    under Settings -> Actions -> Runners, then: rm -rf $dir"
    kept=$((kept + 1))
    continue
  fi
  rm -rf "$dir"
done

if command -v gh >/dev/null 2>&1; then
  echo "Runners still registered to $REPO:"
  gh api "repos/$REPO/actions/runners" --jq '.runners[].name' || true
fi
[ "$kept" -eq 0 ]
