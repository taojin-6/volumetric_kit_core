#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
#
# Registers one repository's self-hosted runners on this host -- Linux GPU
# hosts (label vk-linux-gpu) or Macs (label mac). README.md has the
# prerequisites and the layout.
#
#   bash tools/runners/setup-runners.sh [--user <account>] <recon|gfx|calib|player|owner/repo>
#
# Run as the user that owns ~/ci-runners, in a terminal or over SSH; it calls
# sudo for the services. With --user, the runners are another account's, in
# its home, and run as it (README.md, "A dedicated account"). Safe to re-run:
# registered runners are kept, so no new token is needed, a stopped one is
# started, and a running one restarts only when its .env changed. A
# MoltenVK_ROOT the shell sets goes into each runner's .env, for the iOS
# builds (README.md, "Macs"). Registering needs gh, authed, which first checks
# that only the repository's owner can trigger its workflows (README.md, "Who
# can trigger CI").
set -euo pipefail

# shellcheck source-path=SCRIPTDIR source=common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

# gh_get PATH JQ [OPTION...]: GitHub's answer to a GET of PATH, through gh's
# --jq JQ, or fails, naming PATH, after gh's own error.
gh_get() {
  local path="$1" jq="$2"
  shift 2
  gh api "$@" "$path" --jq "$jq" || {
    echo "error: gh could not read $path (above)" >&2
    return 1
  }
}

# The verdict on one Actions policy, with OWNER_ID replaced by the owner's
# account ID: "ok" when it is active, covers every workflow and lets only the
# owner trigger them; "others" when it is active and lets any other actor
# trigger one; "skip" otherwise. Each is followed by the policy and what was
# found. A condition covers every workflow when it includes "~ALL", or
# includes nothing (GitHub: every workflow no exclusion matches), and
# excludes nothing.
# shellcheck disable=SC2016 # jq's own variables
POLICY_JQ='[.rules[]? | select(.type == "restrict_actions_actors") | .parameters.allowed_actors // []] as $lists
  | (.conditions // {}) as $c
  | "policy \(.id) \(.name | @json)" as $p
  | if .enforcement != "active" then "skip \($p) is not active (enforcement: \(.enforcement))"
    elif ($lists | length) == 0 then "skip \($p) restricts no actors"
    elif any($lists[][]; .type != "User" or .id != OWNER_ID) then "others \($p) lets actors other than the owner trigger workflows"
    elif any($lists[]; length == 0) then "skip \($p) names no actor"
    elif ($c | type) != "object" or ($c.workflow_path | type) != "object"
      or any($c[]; type != "object" or (.include | type) != "array"
        or ((.include | length) > 0 and (any(.include[]; . == "~ALL") | not)) or ([.exclude[]?] | length) > 0)
    then "skip \($p) does not cover every workflow"
    else "ok \($p)" end'

# check_triggers: returns when no one but $REPO's owner can send a job to its
# runners, and otherwise exits, saying what to set and how (README.md, "Who
# can trigger CI"). Any repository needs an active Actions policy over every
# workflow that lets only the owner trigger them, and none that lets another
# actor trigger any: a collaborator's push, as a fork's pull request, runs
# its own workflow files. A public one also needs approval of every outside
# contributor's run. A private one, which has no such approval setting (GitHub
# answers 422), must not run fork pull requests' workflows, when that is
# readable.
check_triggers() {
  local info private owner_id owner ids id verdict ok=0 found="" problems="" approval forks why
  info="$(gh_get "repos/$REPO" '"\(.private) \(.owner.id) \(.owner.login)"')" || exit 1
  read -r private owner_id owner <<<"$info"
  { [ "$private" = true ] || [ "$private" = false ]; } && [[ $owner_id =~ ^[0-9]+$ ]] ||
    die "unexpected answer from GitHub about $REPO: '$info'"

  ids="$(gh_get "repos/$REPO/actions/policies?per_page=100" '.policies[].id' --paginate)" || exit 1
  for id in $ids; do
    [[ $id =~ ^[0-9]+$ ]] || die "unexpected Actions policy ID of $REPO: '$id'"
    verdict="$(gh_get "repos/$REPO/actions/policies/$id" "${POLICY_JQ//OWNER_ID/$owner_id}")" || exit 1
    found="$found
    ${verdict#* }"
    case "$verdict" in
      ok\ *) ok=$((ok + 1)) ;;
      others\ *)
        problems="$problems
- Its ${verdict#* }.
  Remove them from its allowed actors, or delete it, at
  https://github.com/$REPO/settings/actions/rules/$id, then re-run."
        ;;
    esac
  done
  if [ "$ok" -eq 0 ]; then
    problems="$problems
- No active Actions policy over every workflow lets only $owner trigger them.
  Its policies:${found:- none}
  Add the owner-only policy, then re-run:
    gh api -X POST repos/$REPO/actions/policies --input - <<<'{\"name\": \"Only the owner can trigger workflows\", \"enforcement\": \"active\", \"conditions\": {\"workflow_path\": {\"include\": [\"~ALL\"], \"exclude\": []}}, \"rules\": [{\"type\": \"restrict_actions_actors\", \"parameters\": {\"allowed_actors\": [{\"id\": $owner_id, \"type\": \"User\"}]}}]}'"
  fi

  if [ "$private" = true ]; then
    why="is private, but a collaborator's push or fork's pull request runs its own"
    if forks="$(gh api "repos/$REPO/actions/permissions/fork-pr-workflows-private-repos" \
      --jq .run_workflows_from_fork_pull_requests 2>/dev/null)"; then
      [ "$forks" != true ] || problems="$problems
- It runs fork pull requests' workflows, which would reach these runners.
  Turn that off, then re-run:
    gh api -X PUT repos/$REPO/actions/permissions/fork-pr-workflows-private-repos -F run_workflows_from_fork_pull_requests=false"
    else
      echo "Could not read whether $REPO runs fork pull requests' workflows; it is private, so going on."
    fi
  else
    why="is public, and a fork's pull request runs its own"
    approval="$(gh_get "repos/$REPO/actions/permissions/fork-pr-contributor-approval" .approval_policy)" || exit 1
    if [ "$approval" != all_external_contributors ]; then
      problems="$problems
- Fork pull requests' runs wait for approval only from $approval,
  not from every outside contributor. Require it for all of them, then re-run:
    gh api -X PUT repos/$REPO/actions/permissions/fork-pr-contributor-approval -f approval_policy=all_external_contributors"
    fi
  fi
  [ -z "$problems" ] || die "$REPO $why
workflow files, so no runner is registered until only the owner can send a job to
them (README.md, \"Who can trigger CI\"):$problems"
}

select_args "$@"
if [ -n "${MoltenVK_ROOT:-}" ]; then
  # Absolute: the jobs run in the runners' own directories.
  case "$MoltenVK_ROOT" in
    /*) ;;
    *) die "MoltenVK_ROOT is not an absolute path: $MoltenVK_ROOT" ;;
  esac
  as_runner test -d "$MoltenVK_ROOT" || die "MoltenVK_ROOT is not a directory: $MoltenVK_ROOT"
fi
# Registering takes over any runner of the same name, and this user's own
# runners of the repository carry the names the account's would get, so they
# are torn down first.
own_runners() {
  local RUNNER_USER="$USER" RUNNER_HOME="$HOME" RUNNER_ROOT="$HOME/ci-runners/${REPO#*/}"
  find_runner_dirs
  [ "${#RUNNER_DIRS[@]}" -gt 0 ]
}
if [ "$RUNNER_USER" != "$USER" ] && own_runners; then
  die "$USER has runners of $REPO here: tear them down first, then set up $RUNNER_USER's"
fi

case "$(uname -m)" in
  x86_64) PKG_ARCH=x64 ;;
  aarch64 | arm64) PKG_ARCH=arm64 ;;
  *) die "unsupported architecture: $(uname -m)" ;;
esac
case "$OS" in
  Linux)
    PKG_OS=linux
    LABEL=vk-linux-gpu
    # A run of recon, gfx or player has at most 2 Linux jobs: 6 fit three
    # runs on one host while the other is offline.
    N=6
    CORES="$(nproc)"
    command -v docker >/dev/null 2>&1 ||
      die "install Docker Engine first: https://docs.docker.com/engine/install/ubuntu/"
    # The legs would still run without the toolkit, but on lavapipe inside the
    # container rather than the GPU's driver.
    if ! command -v nvidia-ctk >/dev/null 2>&1 || ! docker info 2>/dev/null | grep -qi nvidia; then
      echo "WARNING: the NVIDIA Container Toolkit is not wired into Docker, so the GPU"
      echo "  will not reach the job containers (README.md, 'Linux hosts'). Continuing."
    fi
    ;;
  Darwin)
    PKG_OS=osx
    LABEL=mac
    N=2 # Debug and Release in parallel
    CORES="$(sysctl -n hw.ncpu)"
    xcode-select -p >/dev/null 2>&1 || die "install Xcode first (README.md, 'Macs')"
    command -v brew >/dev/null 2>&1 || die "install Homebrew first: https://brew.sh"
    if ! xcodebuild -version >/dev/null 2>&1; then
      echo "WARNING: xcodebuild does not run (Command Line Tools only?); a leg that"
      echo "  generates with -G Xcode needs full Xcode (README.md, 'Macs'). Continuing."
    fi
    ;;
  *)
    echo "$REPO has no self-hosted runners on $OS, only on Linux and Darwin (README.md)" >&2
    exit 2
    ;;
esac

# The repository's runners already registered here, in either layout. New
# ones are registered only up to N in all, so a host whose runners are still
# in ~/actions-runner-* gets no second set. Who can trigger the workflows is
# checked, and a token and the runner fetched, only for that, so a re-run that
# only refreshes the services needs none of them, nor gh.
find_runner_dirs
have=${#RUNNER_DIRS[@]}
if [ "$have" -lt "$N" ]; then
  if ! command -v gh >/dev/null 2>&1 || ! gh auth status >/dev/null 2>&1; then
    die "registering needs gh, authed (gh auth login, or GH_TOKEN set): it checks who can trigger $REPO's workflows and mints the token"
  fi
  check_triggers
  # The latest runner release, from the tag its page redirects to: GitHub
  # stops registering a release some time after the next one ships.
  VER="$(curl -fsSIL -o /dev/null -w '%{url_effective}' https://github.com/actions/runner/releases/latest)"
  VER="${VER##*/v}"
  case "$VER" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) die "could not find the runner's latest release (got '$VER')" ;;
  esac
  TOKEN="$(gh api -X POST "repos/$REPO/actions/runners/registration-token" --jq .token)"
  as_runner mkdir -p "$BASE"
  TAR="$BASE/actions-runner-$PKG_OS-$PKG_ARCH-$VER.tar.gz"
  # Reuse a cached tarball only if it is a whole archive: an interrupted
  # download leaves a truncated file that tar would choke on.
  as_runner tar tzf "$TAR" >/dev/null 2>&1 || as_runner curl -fsSL -o "$TAR" \
    "https://github.com/actions/runner/releases/download/v$VER/actions-runner-$PKG_OS-$PKG_ARCH-$VER.tar.gz"
fi

# A new runner takes the lowest number no runner of the repository here holds
# and is named <host>-<slug>-<number>, since one host serves several
# repositories. recon's pre-layout runners, ~/actions-runner-recon-<i>, carry
# the same names, so their numbers are taken too. --replace reclaims the
# registration of a runner of that name whose directory is gone; it must
# never meet a working one, which it would disconnect.
i=0
while [ "$have" -lt "$N" ]; do
  i=$((i + 1))
  dir="$RUNNER_ROOT/runner-$i"
  if as_runner test -f "$dir/.runner" || as_runner test -d "$RUNNER_HOME/actions-runner-$SLUG-$i"; then continue; fi
  echo "==> Registering $dir"
  as_runner mkdir -p "$dir"
  as_runner tar xzf "$TAR" -C "$dir"
  in_dir "$dir" ./config.sh --unattended --url "https://github.com/$REPO" \
    --token "$TOKEN" --labels "$LABEL" --name "$(hostname -s)-$SLUG-$i" \
    --work _work --replace
  have=$((have + 1))
done
find_runner_dirs

# The parallel levels split the host's cores across the repository's runners,
# in either layout; other repositories' runners keep theirs (DECISIONS.md,
# 2026-10-16). They reach jobs that run on the host, not job containers, whose
# workflows set their own.
THREADS=$((CORES / ${#RUNNER_DIRS[@]}))
[ "$THREADS" -ge 1 ] || THREADS=1
broken=0
for dir in "${RUNNER_DIRS[@]}"; do
  echo "==> $dir"
  ENV_CHANGED=0
  set_env "$dir/.env" CMAKE_BUILD_PARALLEL_LEVEL "$THREADS"
  set_env "$dir/.env" CTEST_PARALLEL_LEVEL "$THREADS"
  if [ -n "${MoltenVK_ROOT:-}" ]; then set_env "$dir/.env" MoltenVK_ROOT "$MoltenVK_ROOT"; fi
  # The runner reads .env only when it starts, so a running one restarts; a
  # stopped one reads it when the loop below starts it.
  if [ "$ENV_CHANGED" -eq 1 ] && service_running "$dir"; then
    echo "    .env changed; restarting the runner"
    restart_service "$dir" || broken=$((broken + 1))
  fi
done

# Each of the repository's runners is a service started at boot: installed if
# it is not, and started if it is not running. On macOS a LaunchAgent of an
# earlier setup goes once the daemon is installed, before it starts.
for dir in "${RUNNER_DIRS[@]}"; do
  if ! service_installed "$dir"; then
    echo "==> Installing the service of $dir"
    install_service "$dir" || {
      broken=$((broken + 1))
      continue
    }
  fi
  remove_launch_agent "$dir" || {
    broken=$((broken + 1))
    continue
  }
  service_running "$dir" || start_service "$dir" || broken=$((broken + 1))
done
[ "$broken" -eq 0 ] || die "$broken runner services failed (above): fix the cause and re-run"

echo "Done: ${#RUNNER_DIRS[@]} runners of $REPO labelled '$LABEL', $THREADS build threads each, run as $RUNNER_USER."
if [ "$OS" = Linux ]; then
  echo "Check they are running: systemctl list-units 'actions.runner.*' --no-pager"
else
  echo "Check they are running: sudo launchctl list | grep actions.runner"
  echo "Keep this Mac awake: sudo pmset -a sleep 0 disablesleep 1"
fi
