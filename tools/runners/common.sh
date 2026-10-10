# shellcheck shell=bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
#
# Sourced by setup-runners.sh and teardown-runners.sh: which repository a
# runner belongs to, where its files live, and how its service is driven.
# README.md describes the layout.

# Every repository's runners live under one directory, each repository's in its
# own subdirectory, so several repositories share a host without colliding.
BASE="$HOME/ci-runners"
OS="$(uname -s)"

die() {
  echo "error: $*" >&2
  exit 1
}

# Sets REPO, SLUG and RUNNER_ROOT for a repository's short name, or exits 2.
select_repo() {
  case "${1:-}" in
    recon | gfx) ;;
    *)
      echo "unknown repository '${1:-}': expected recon or gfx" >&2
      exit 2
      ;;
  esac
  # shellcheck disable=SC2034 # setup-runners.sh names runners with it
  SLUG="$1"
  REPO="taojin-6/volumetric_kit_$1"
  RUNNER_ROOT="$BASE/${REPO#*/}"
}

# Runs a command as root on Linux, where each runner is a systemd service, and
# as this user on macOS, where it is a per-user LaunchAgent.
as_admin() {
  if [ "$OS" = Linux ]; then
    sudo "$@"
  else
    "$@"
  fi
}

# svc DIR ARGS...: the runner's own service script, run from its directory.
svc() {
  local dir="$1"
  shift
  (cd "$dir" && as_admin ./svc.sh "$@")
}

# Whether runner directory $1 is registered to $REPO. Its .runner records the
# repository URL; the closing quote stops one name matching another's prefix.
registered_to_repo() {
  grep -qsF "github.com/${REPO}\"" "${1%/}/.runner"
}

# Fills RUNNER_DIRS with this repository's registered runners on this host:
# each ~/ci-runners/<repo>/runner-*, and each flat ~/actions-runner-* of hosts
# set up before that layout, whose .runner names $REPO -- that glob matches
# every repository's.
find_runner_dirs() {
  RUNNER_DIRS=()
  local dir
  for dir in "$RUNNER_ROOT"/runner-*/ "$HOME"/actions-runner-*/; do
    if registered_to_repo "$dir"; then RUNNER_DIRS+=("${dir%/}"); fi
  done
}

# set_env FILE KEY VALUE: makes KEY=VALUE the only KEY line in FILE, in place
# of the first old one, keeping every other line. Sets ENV_CHANGED=1 when the
# file changes.
set_env() {
  local file="$1" key="$2" value="$3" tmp
  touch "$file"
  tmp="$(mktemp "${file}.XXXXXX")"
  awk -v k="$key" -v v="$value" '
    index($0, k "=") == 1 { if (!done) print k "=" v; done = 1; next }
    { print }
    END { if (!done) print k "=" v }
  ' "$file" >"$tmp"
  if cmp -s "$tmp" "$file"; then
    rm -f "$tmp"
  else
    mv -f "$tmp" "$file"
    # shellcheck disable=SC2034 # setup-runners.sh reads it
    ENV_CHANGED=1
  fi
}

# Whether runner directory $1's service is running. svc.sh exits 0 either way,
# so this reads what it prints: systemd's state on Linux, and on macOS whether
# launchd has the agent loaded.
service_running() {
  local out
  [ -f "$1/.service" ] || return 1 # svc.sh install writes it; uninstall deletes it
  out="$(svc "$1" status 2>/dev/null)" || return 1
  case "$OS:$out" in
    Linux:*"Active: active "* | Darwin:*Started:*) return 0 ;;
  esac
  return 1
}

# Starts runner directory $1's service and checks that it runs: on macOS
# svc.sh exits 0 even when launchctl fails.
start_service() {
  svc "$1" start >/dev/null
  service_running "$1" && return 0
  echo "error: the runner in $1 did not start; './svc.sh status' there says why" >&2
  return 1
}
