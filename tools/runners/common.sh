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

# Fills RUNNER_DIRS with this repository's runner directories on this host:
# every ~/ci-runners/<repo>/runner-*, and the flat ~/actions-runner-* of hosts
# set up before that layout, but only those whose .runner names $REPO -- that
# glob matches every repository's.
find_runner_dirs() {
  RUNNER_DIRS=()
  local dir
  for dir in "$RUNNER_ROOT"/runner-*/ "$HOME"/actions-runner-*/; do
    [ -f "${dir}config.sh" ] || continue
    case "$dir" in
      "$RUNNER_ROOT"/*) RUNNER_DIRS+=("${dir%/}") ;;
      *) registered_to_repo "$dir" && RUNNER_DIRS+=("${dir%/}") ;;
    esac
  done
  return 0
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
    ENV_CHANGED=1
  fi
}

# configure_runner_env DIR KEY=VALUE...: sets each pair in runner directory
# DIR's .env, restarting the runner if the file changed: the runner reads .env
# only when it starts.
configure_runner_env() {
  local dir="$1" pair
  shift
  ENV_CHANGED=0
  for pair in "$@"; do
    set_env "$dir/.env" "${pair%%=*}" "${pair#*=}"
  done
  if [ "$ENV_CHANGED" -eq 1 ]; then
    echo "    .env changed; restarting the runner"
    svc "$dir" stop || true
    svc "$dir" start
  fi
}
