#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
#
# Registers one repository's self-hosted runners on this host -- Linux GPU
# hosts (label vk-linux-gpu) or Macs (label mac). README.md has the
# prerequisites and the layout.
#
#   bash tools/runners/setup-runners.sh <recon|gfx>
#
# Run as the user that owns ~/ci-runners (on Linux it calls sudo for the
# services). Safe to re-run: registered runners are kept, so no new token is
# needed, a stopped one is started, and a running one restarts only when its
# .env changed.
set -euo pipefail

# shellcheck source-path=SCRIPTDIR source=common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

[ $# -eq 1 ] || {
  echo "usage: $0 <recon|gfx>" >&2
  exit 2
}
select_repo "$1"

case "$(uname -m)" in
  x86_64) PKG_ARCH=x64 ;;
  aarch64 | arm64) PKG_ARCH=arm64 ;;
  *) die "unsupported architecture: $(uname -m)" ;;
esac
case "$OS" in
  Linux)
    PKG_OS=linux
    LABEL=vk-linux-gpu
    N=6 # 3 Linux legs per run; 6 let two runs, or both hosts' legs, proceed
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
  *) die "unsupported OS: $OS" ;;
esac

# The repository's runners already registered here, in either layout. New
# ones are registered only up to N in all, so a host whose runners are still
# in ~/actions-runner-* gets no second set. A token and the runner are fetched
# only for that, so a re-run that only refreshes the services needs neither.
find_runner_dirs
have=${#RUNNER_DIRS[@]}
if [ "$have" -lt "$N" ]; then
  # The latest runner release, from the tag its page redirects to: GitHub
  # stops registering a release some time after the next one ships.
  VER="$(curl -fsSIL -o /dev/null -w '%{url_effective}' https://github.com/actions/runner/releases/latest)"
  VER="${VER##*/v}"
  case "$VER" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) die "could not find the runner's latest release (got '$VER')" ;;
  esac
  # Mint a registration token via gh if it is authed; otherwise ask for one.
  if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
    TOKEN="$(gh api -X POST "repos/$REPO/actions/runners/registration-token" --jq .token)"
  else
    echo "gh is not authed: get a token at https://github.com/$REPO/settings/actions/runners/new"
    read -r -p "Paste registration token: " TOKEN
  fi
  mkdir -p "$BASE"
  TAR="$BASE/actions-runner-$PKG_OS-$PKG_ARCH-$VER.tar.gz"
  # Reuse a cached tarball only if it is a whole archive: an interrupted
  # download leaves a truncated file that tar would choke on.
  tar tzf "$TAR" >/dev/null 2>&1 || curl -fsSL -o "$TAR" \
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
  if [ -f "$dir/.runner" ] || [ -d "$HOME/actions-runner-$SLUG-$i" ]; then continue; fi
  echo "==> Registering $dir"
  mkdir -p "$dir"
  tar xzf "$TAR" -C "$dir"
  (cd "$dir" && ./config.sh --unattended --url "https://github.com/$REPO" \
    --token "$TOKEN" --labels "$LABEL" --name "$(hostname -s)-$SLUG-$i" \
    --work _work --replace)
  have=$((have + 1))
done
find_runner_dirs

# The parallel levels split the host's cores across every runner on it, of
# every repository and in either layout, so they are written into all of
# them: adding a repository's runners shrinks the others' share. They reach
# jobs that run on the host, not job containers, whose workflows set their own.
HOST_RUNNER_DIRS=()
for dir in "$BASE"/*/runner-*/ "$HOME"/actions-runner-*/; do
  if [ -f "${dir}.runner" ]; then HOST_RUNNER_DIRS+=("${dir%/}"); fi
done
THREADS=$((CORES / ${#HOST_RUNNER_DIRS[@]}))
[ "$THREADS" -ge 1 ] || THREADS=1
broken=0
for dir in "${HOST_RUNNER_DIRS[@]}"; do
  echo "==> $dir"
  ENV_CHANGED=0
  set_env "$dir/.env" CMAKE_BUILD_PARALLEL_LEVEL "$THREADS"
  set_env "$dir/.env" CTEST_PARALLEL_LEVEL "$THREADS"
  # The runner reads .env only when it starts, so a running one restarts. A
  # stopped one, such as another repository's paused runner, stays stopped.
  if [ "$ENV_CHANGED" -eq 1 ] && service_running "$dir"; then
    echo "    .env changed; restarting the runner"
    { svc "$dir" stop >/dev/null && start_service "$dir"; } || broken=$((broken + 1))
  fi
done

# Each of the repository's runners is a service, started at boot (Linux) or
# login (macOS): installed if it is not, and started if it is not running.
for dir in "${RUNNER_DIRS[@]}"; do
  if [ ! -f "$dir/.service" ]; then
    echo "==> Installing the service of $dir"
    if [ "$OS" = Linux ]; then
      svc "$dir" install "$USER" # as this user, not root
    else
      svc "$dir" install
    fi || {
      broken=$((broken + 1))
      continue
    }
  fi
  service_running "$dir" || start_service "$dir" || broken=$((broken + 1))
done
[ "$broken" -eq 0 ] || die "$broken runner services failed (above): fix the cause and re-run"

echo "Done: ${#RUNNER_DIRS[@]} runners of $REPO labelled '$LABEL', $THREADS build threads each."
if [ "$OS" = Linux ]; then
  echo "Check they are running: systemctl list-units 'actions.runner.*' --no-pager"
else
  echo "Keep this Mac awake and logged in: sudo pmset -a sleep 0 disablesleep 1"
fi
