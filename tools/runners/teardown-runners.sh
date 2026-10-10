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
# Run as the user that owns the runner directories, with gh authed, which
# deregistering needs: without it, no registered runner is touched. It keeps
# going past a runner it cannot remove, and exits non-zero if any is left.
set -uo pipefail

# shellcheck source-path=SCRIPTDIR source=common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

[ $# -eq 1 ] || {
  echo "usage: $0 <recon|gfx>" >&2
  exit 2
}
select_repo "$1"

# The repository's registered runners, in either layout, and its unregistered
# leftovers -- a directory under ~/ci-runners/<repo>/, or one of recon's
# pre-layout ~/actions-runner-recon-*, with no .runner, from a registration or
# removal that did not finish.
find_runner_dirs
leftovers=()
for dir in "$RUNNER_ROOT"/runner-*/ "$HOME/actions-runner-$SLUG"-*/; do
  if [ -d "$dir" ] && [ ! -f "${dir}.runner" ]; then leftovers+=("${dir%/}"); fi
done
if [ "$((${#RUNNER_DIRS[@]} + ${#leftovers[@]}))" -eq 0 ]; then
  echo "No runner of $REPO on this host (looked in $RUNNER_ROOT/ and ~/actions-runner-*/)."
  exit 0
fi

# One remove token serves every runner. It is minted before any runner is
# touched, so without gh auth, or if GitHub refuses, every runner keeps
# running rather than going offline still registered.
if [ "${#RUNNER_DIRS[@]}" -gt 0 ]; then
  if ! { command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; }; then
    die "gh is not authed, so no runner can be deregistered; none was touched. Run 'gh auth login' and re-run."
  fi
  token="$(gh api -X POST "repos/$REPO/actions/runners/remove-token" --jq .token)" || token=""
  [ -n "$token" ] || die "could not mint a remove token; no runner was touched"
fi

# remove_service DIR: uninstalls the runner's service, which stops it first.
remove_service() {
  if [ -f "$1/.service" ]; then svc "$1" uninstall >/dev/null; fi
}

kept=0
for dir in ${RUNNER_DIRS[@]+"${RUNNER_DIRS[@]}"}; do # empty: set -u, bash 3.2
  echo "==> Removing $dir"
  # The runner refuses to deregister while its service is installed. Its
  # directory is deleted only once it is deregistered: it holds the
  # credentials deregistering needs, so deleting it first would strand an
  # offline registration that nothing on this host can remove.
  if ! { remove_service "$dir" && (cd "$dir" && ./config.sh remove --token "$token"); }; then
    echo "    Kept $dir (still registered, and offline): re-run, or remove it under"
    echo "    Settings -> Actions -> Runners and then: rm -rf $dir"
    kept=$((kept + 1))
    continue
  fi
  rm -rf "$dir"
done
for dir in ${leftovers[@]+"${leftovers[@]}"}; do
  echo "==> Removing $dir (not registered)"
  remove_service "$dir" || true
  rm -rf "$dir"
done

if command -v gh >/dev/null 2>&1; then
  echo "Runners still registered to $REPO:"
  gh api "repos/$REPO/actions/runners" --jq '.runners[].name' || true
fi
[ "$kept" -eq 0 ]
