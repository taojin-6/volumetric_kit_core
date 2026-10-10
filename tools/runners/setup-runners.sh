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
# needed, and a runner restarts only when its .env changed.
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
have=0
for dir in ${RUNNER_DIRS[@]+"${RUNNER_DIRS[@]}"}; do # empty: set -u, bash 3.2
  if [ -f "$dir/.runner" ]; then have=$((have + 1)); fi
done
if [ "$have" -lt "$N" ]; then
  # Mint a registration token via gh if it is authed; otherwise ask for one.
  if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
    VER="$(gh api repos/actions/runner/releases/latest --jq .tag_name | sed 's/^v//')"
    TOKEN="$(gh api -X POST "repos/$REPO/actions/runners/registration-token" --jq .token)"
  else
    echo "gh is not authed: get a token at https://github.com/$REPO/settings/actions/runners/new"
    VER="2.335.1"
    read -r -p "Paste registration token: " TOKEN
  fi
  mkdir -p "$BASE"
  TAR="$BASE/actions-runner-$PKG_OS-$PKG_ARCH-$VER.tar.gz"
  # Reuse a cached tarball only if it is a whole archive: an interrupted
  # download leaves a truncated file that tar would choke on.
  tar tzf "$TAR" >/dev/null 2>&1 || curl -fsSL -o "$TAR" \
    "https://github.com/actions/runner/releases/download/v$VER/actions-runner-$PKG_OS-$PKG_ARCH-$VER.tar.gz"
fi

for i in $(seq 1 "$N"); do
  dir="$RUNNER_ROOT/runner-$i"
  echo "==> [$i/$N] $dir"
  if [ -f "$dir/.runner" ]; then
    echo "    already registered"
  elif [ "$have" -ge "$N" ]; then
    echo "    skipped: $N runners of $REPO are registered on this host"
    continue
  else
    have=$((have + 1))
    mkdir -p "$dir"
    tar xzf "$TAR" -C "$dir"
    # Runners registered before this script keep their names; new ones carry
    # the repository's slug, since one host serves several repositories.
    (cd "$dir" && ./config.sh --unattended --url "https://github.com/$REPO" \
      --token "$TOKEN" --labels "$LABEL" --name "$(hostname -s)-$SLUG-$i" \
      --work _work --replace)
  fi
  # One service per runner, started at boot (Linux) or login (macOS).
  # 'install' fails when the service already exists; that is fine.
  if [ "$OS" = Linux ]; then
    svc "$dir" install "$USER" >/dev/null 2>&1 || true
  else
    svc "$dir" install >/dev/null 2>&1 || true
  fi
done

# Every registered runner of this repository, including any in the flat
# pre-layout directories, gets the parallel levels. They split the host's
# cores across every repository's runners on it, in either layout; they reach
# jobs that run on the host, not job containers, whose workflows set their own.
find_runner_dirs
shared=0
for marker in "$BASE"/*/runner-*/.runner "$HOME"/actions-runner-*/.runner; do
  if [ -f "$marker" ]; then shared=$((shared + 1)); fi
done
[ "$shared" -ge 1 ] || shared=1
THREADS=$((CORES / shared))
[ "$THREADS" -ge 1 ] || THREADS=1
for dir in "${RUNNER_DIRS[@]}"; do
  echo "==> $dir"
  configure_runner_env "$dir" "CMAKE_BUILD_PARALLEL_LEVEL=$THREADS" "CTEST_PARALLEL_LEVEL=$THREADS"
  # A runner whose .env did not change is started if it is not running.
  [ "$ENV_CHANGED" -eq 1 ] || svc "$dir" start >/dev/null 2>&1 || true
done

echo "Done: ${#RUNNER_DIRS[@]} runners of $REPO labelled '$LABEL', $THREADS build threads each."
if [ "$OS" = Linux ]; then
  echo "Check they are running: systemctl list-units 'actions.runner.*' --no-pager"
else
  echo "Keep this Mac awake and logged in: sudo pmset -a sleep 0 disablesleep 1"
fi
