#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
#
# Runs setup-runners.sh and teardown-runners.sh against a fake HOME, with
# stubs in place of the runner's config.sh and svc.sh, gh, curl, sudo and the
# host prerequisites, and checks what they did to which runner. No real runner
# or service is touched. The scripts run under the bash running this, so
# `/bin/bash test_runner_scripts.sh` tests them under macOS's bash 3.2.
# shellcheck disable=SC2016 # the stubs' variables expand when the stubs run
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tools="$here/.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
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

# --- Stubs ------------------------------------------------------------------

stubs="$tmp/stubs"
runner_files="$tmp/runner-files"
mkdir -p "$stubs" "$runner_files"
export CALLS="$tmp/calls.log" RUNNER_FILES="$runner_files"

# stub NAME BODY: an executable NAME on the stubs' PATH.
stub() {
  printf '#!/bin/sh\n%s\n' "$2" >"$stubs/$1"
  chmod +x "$stubs/$1"
}
stub sudo 'exec "$@"'
stub curl '
echo "curl $*" >>"$CALLS"
case "$*" in
  *url_effective*) printf %s https://github.com/actions/runner/releases/tag/v9.9.9 ;;
  *) exit 1 ;;
esac'
stub docker '[ "$1" = info ] && echo " Runtimes: nvidia runc"; exit 0'
stub nvidia-ctk 'exit 0'
stub xcode-select 'echo /Applications/Xcode.app/Contents/Developer'
stub xcodebuild 'echo Xcode'
stub brew 'exit 0'
stub gh '
echo "gh $*" >>"$CALLS"
case "$*" in
  "auth status") [ -z "${STUB_GH_UNAUTHED:-}" ] ;;
  *registration-token*) echo REGTOKEN ;;
  *remove-token*) echo RMTOKEN ;;
esac'

# The runner's own scripts. Registering writes .runner and generates svc.sh,
# as the real config.sh does, and deregistering is refused while the service
# is installed. svc.sh keeps the service's state in .service (installed, as
# the real one does) and .running, and prints its status the way the real one
# does on each OS; like the real one on macOS, a failed start exits 0.
printf '%s\n' '#!/bin/sh' \
  'echo "config ${PWD#"$HOME"/} $*" >>"$CALLS"' \
  'if [ "$1" = remove ]; then' \
  '  [ -z "${STUB_REMOVE_FAILS:-}" ] || exit 1' \
  '  [ ! -f .service ] || { echo "Uninstall service first" >&2; exit 1; }' \
  '  rm -f .runner; exit 0' \
  'fi' \
  'while [ $# -gt 0 ]; do [ "$1" = --url ] && url="$2"; shift; done' \
  'printf "{\n  \"gitHubUrl\": \"%s\"\n}\n" "$url" >.runner' \
  'cp "$RUNNER_FILES/svc.sh" .' >"$runner_files/config.sh"
printf '%s\n' '#!/bin/sh' \
  'echo "svc ${PWD#"$HOME"/} $*" >>"$CALLS"' \
  'case "$1" in' \
  '  install) [ -z "${STUB_INSTALL_FAILS:-}" ] && [ ! -f .service ] && echo unit >.service ;;' \
  '  uninstall) rm -f .service .running ;;' \
  '  start) [ -n "${STUB_START_FAILS:-}" ] || [ ! -f .service ] || touch .running ;;' \
  '  stop) rm -f .running ;;' \
  '  status)' \
  '    [ -f .service ] || { echo "not installed"; exit 1; }' \
  '    if [ -f .running ]; then printf "Started:\n   Active: active (running)\n"; else echo Stopped; fi ;;' \
  'esac' >"$runner_files/svc.sh"
chmod +x "$runner_files/config.sh" "$runner_files/svc.sh"

export PATH="$stubs:$PATH"
real_home="$HOME"
user="${USER:-$(id -un)}"
export USER="$user"

# fresh_home: an empty HOME and calls log.
fresh_home() {
  export HOME="$tmp/home"
  rm -rf "$HOME"
  mkdir -p "$HOME"
  : >"$CALLS"
}

# runner DIR REPO [ENV]: a runner directory registered to REPO, its service
# installed and running, as setup-runners.sh leaves one; with REPO empty, one
# that never finished registering. ENV is its .env (none if absent).
runner() {
  mkdir -p "$HOME/$1"
  cp "$runner_files/config.sh" "$HOME/$1/"
  if [ -n "$2" ]; then
    printf '{\n  "gitHubUrl": "https://github.com/taojin-6/volumetric_kit_%s"\n}\n' \
      "$2" >"$HOME/$1/.runner"
    cp "$runner_files/svc.sh" "$HOME/$1/"
    echo unit >"$HOME/$1/.service"
    touch "$HOME/$1/.running"
  fi
  if [ $# -ge 3 ]; then printf '%b' "$3" >"$HOME/$1/.env"; fi
}

calls() { grep -c -E -e "$1" "$CALLS" || true; }
env_of() { cat "$HOME/$1/.env" 2>/dev/null || echo "<none>"; }
running() {
  if [ -f "$HOME/$1/.running" ]; then echo yes; else echo no; fi
}
levels() { printf 'CMAKE_BUILD_PARALLEL_LEVEL=%s\nCTEST_PARALLEL_LEVEL=%s' "$1" "$1"; }

setup() { "$BASH" "$tools/setup-runners.sh" "$@" >"$tmp/out" 2>&1; }
teardown() { "$BASH" "$tools/teardown-runners.sh" "$@" >"$tmp/out" 2>&1; }

# --- Arguments --------------------------------------------------------------

fresh_home
for args in "" "core" "recon --bogus" "recon extra"; do
  status=0
  # shellcheck disable=SC2086 # the arguments are split on purpose
  setup $args || status=$?
  expect "setup-runners.sh rejects '$args'" "$status" 2
done
status=0
teardown || status=$?
expect "teardown-runners.sh needs a repository" "$status" 2

# --- Registration -----------------------------------------------------------

if [ "$(uname -s)" = Linux ]; then
  n=6 label=vk-linux-gpu cores="$(nproc)" install="install $user"
else
  n=2 label=mac cores="$(sysctl -n hw.ncpu)" install="install"
fi
host="$(hostname -s)"
# share RUNNERS: the build levels with the cores split across RUNNERS.
share() {
  local threads=$((cores / $1))
  [ "$threads" -ge 1 ] || threads=1
  echo "$threads"
}
# cache_tarball: a cached runner tarball for the version the release page
# redirects to, so nothing downloads. Like the real one, it has no svc.sh.
mkdir -p "$tmp/tarball"
cp "$runner_files/config.sh" "$tmp/tarball/"
cache_tarball() {
  local os arch
  mkdir -p "$HOME/ci-runners"
  for os in linux osx; do
    for arch in x64 arm64; do
      tar czf "$HOME/ci-runners/actions-runner-$os-$arch-9.9.9.tar.gz" -C "$tmp/tarball" .
    done
  done
}

fresh_home
runner ci-runners/volumetric_kit_recon/runner-1 recon \
  'CTEST_PARALLEL_LEVEL=999\nLANG=C.UTF-8\nCTEST_PARALLEL_LEVEL=999\n'
runner ci-runners/volumetric_kit_gfx/runner-1 gfx 'LANG=C\n'
runner ci-runners/volumetric_kit_gfx/runner-2 gfx 'LANG=C\n'
rm "$HOME/ci-runners/volumetric_kit_gfx/runner-2/.running" # paused
cache_tarball
status=0
setup recon || status=$?
expect "registration succeeds" "$status" 0
[ "$status" -eq 0 ] || sed 's/^/     | /' "$tmp/out"
expect "one registration token is minted" "$(calls registration-token)" 1
expect "nothing is downloaded when the tarball is cached" "$(calls '^curl .*/download/')" 0
expect "a registered runner is not registered again" \
  "$(calls '^config ci-runners/volumetric_kit_recon/runner-1 ')" 0
registered=0
for i in $(seq 2 "$n"); do
  grep -qF "config ci-runners/volumetric_kit_recon/runner-$i --unattended --url https://github.com/taojin-6/volumetric_kit_recon --token REGTOKEN --labels $label --name $host-recon-$i " "$CALLS" &&
    registered=$((registered + 1))
done
expect "the missing runners are registered, named and labelled" "$registered" "$((n - 1))"
expect "each new runner's service is installed" \
  "$(calls "^svc ci-runners/volumetric_kit_recon/runner-[0-9]* $install\$")" "$((n - 1))"
up=0
for i in $(seq 1 "$n"); do
  [ "$(running "ci-runners/volumetric_kit_recon/runner-$i")" = no ] || up=$((up + 1))
done
expect "every runner of the repository runs" "$up" "$n"
threads="$(share $((n + 2)))"
expect "the cores are split across every repository's runners; an old level is replaced in place, once, and other lines kept" \
  "$(env_of ci-runners/volumetric_kit_recon/runner-1)" "CTEST_PARALLEL_LEVEL=$threads
LANG=C.UTF-8
CMAKE_BUILD_PARALLEL_LEVEL=$threads"
expect "a new runner gets the same levels" \
  "$(env_of "ci-runners/volumetric_kit_recon/runner-$n")" "$(levels "$threads")"
expect "a running runner whose .env changed restarts" \
  "$(calls '^svc ci-runners/volumetric_kit_recon/runner-1 stop$')" 1
expect "another repository's runners get the share too, keeping their other lines" \
  "$(env_of ci-runners/volumetric_kit_gfx/runner-1) $(env_of ci-runners/volumetric_kit_gfx/runner-2)" \
  "LANG=C
$(levels "$threads") LANG=C
$(levels "$threads")"
expect "another repository's running runner restarts, and its paused one stays paused" \
  "$(calls '^svc ci-runners/volumetric_kit_gfx/runner-1 stop$') $(running ci-runners/volumetric_kit_gfx/runner-1) $(running ci-runners/volumetric_kit_gfx/runner-2)" \
  "1 yes no"

: >"$CALLS"
status=0
setup recon || status=$?
expect "a re-run succeeds" "$status" 0
expect "a re-run asks gh for nothing and registers nothing" "$(calls '^(config|gh|curl) ')" 0
expect "a re-run installs, stops and starts nothing" "$(calls ' (install|stop|start)')" 0

# A paused runner of the repository is started by a re-run, and a service
# that does not install or start fails the run, which still sets up the rest.
rm "$HOME/ci-runners/volumetric_kit_recon/runner-1/.running"
: >"$CALLS"
status=0
setup recon || status=$?
expect "a re-run starts the repository's paused runner" \
  "$status $(running ci-runners/volumetric_kit_recon/runner-1)" "0 yes"
for case in STUB_INSTALL_FAILS STUB_START_FAILS; do
  fresh_home
  for i in $(seq 1 "$n"); do runner "ci-runners/volumetric_kit_recon/runner-$i" recon; done
  rm "$HOME/ci-runners/volumetric_kit_recon/runner-1/.service" \
    "$HOME/ci-runners/volumetric_kit_recon/runner-1/.running"
  status=0
  env "$case=1" "$BASH" "$tools/setup-runners.sh" recon >"$tmp/out" 2>&1 || status=$?
  expect "setup fails when a service does not come up ($case)" "$status" 1
  expect "it still sets up the other runners ($case)" \
    "$(env_of "ci-runners/volumetric_kit_recon/runner-$n")" "$(levels "$(share "$n")")"
done

# A host whose runners are still in the pre-layout directories gets no second
# set, and its thread split counts them, every repository's. One whose service
# is gone is reinstalled, and an unregistered leftover is left alone.
fresh_home
for i in $(seq 1 "$n"); do runner "actions-runner-recon-$i" recon; done
rm "$HOME/actions-runner-recon-1/.service" "$HOME/actions-runner-recon-1/.running"
runner actions-runner-1 gfx 'LANG=C\n'
runner ci-runners/volumetric_kit_recon/runner-1 ""
status=0
setup recon || status=$?
expect "a full run on a pre-layout host succeeds" "$status" 0
[ "$status" -eq 0 ] || sed 's/^/     | /' "$tmp/out"
expect "it asks gh for nothing and registers nothing" "$(calls '^(config|gh|curl) ')" 0
expect "it counts only registered runners" \
  "$(grep -c "^Done: $n runners " "$tmp/out" || true)" 1
expect "it leaves an unregistered leftover alone" \
  "$(calls '^svc ci-runners/') $(env_of ci-runners/volumetric_kit_recon/runner-1)" "0 <none>"
expect "it reinstalls and starts a pre-layout runner's service" \
  "$(calls "^svc actions-runner-recon-1 $install\$") $(running actions-runner-recon-1)" "1 yes"
threads="$(share $((n + 1)))"
expect "they get a split across every pre-layout runner" \
  "$(env_of "actions-runner-recon-$n")" "$(levels "$threads")"
expect "another repository's pre-layout runner gets it too" \
  "$(env_of actions-runner-1)" "LANG=C
$(levels "$threads")"

fresh_home
for i in $(seq 2 "$n"); do runner "actions-runner-recon-$i" recon; done
cache_tarball
status=0
setup recon || status=$?
expect "a pre-layout host short of runners succeeds" "$status" 0
[ "$status" -eq 0 ] || sed 's/^/     | /' "$tmp/out"
expect "it registers only the missing one" \
  "$(calls '^config ci-runners/volumetric_kit_recon/runner-1 ') $(calls '^config ')" "1 1"

# recon's pre-layout runners have the names new ones get, so a new runner
# must not take a number one of them holds: --replace would take over its
# registration.
fresh_home
for i in $(seq 1 "$n"); do
  [ "$i" -eq 2 ] || runner "actions-runner-recon-$i" recon
done
cache_tarball
status=0
setup recon || status=$?
expect "a pre-layout host short of a runner other than the first succeeds" "$status" 0
[ "$status" -eq 0 ] || sed 's/^/     | /' "$tmp/out"
expect "it names the new runner after a free number, not a working runner's" \
  "$(calls "^config ci-runners/volumetric_kit_recon/runner-2 .* --name $host-recon-2 ") $(calls '^config ')" "1 1"

# --- Teardown ---------------------------------------------------------------

fresh_home
runner ci-runners/volumetric_kit_recon/runner-1 recon
runner ci-runners/volumetric_kit_recon/runner-2 ""
runner actions-runner-recon-3 recon
runner actions-runner-recon-4 "" # its service left installed
cp "$runner_files/svc.sh" "$HOME/actions-runner-recon-4/"
echo unit >"$HOME/actions-runner-recon-4/.service"
runner actions-runner-1 gfx
runner ci-runners/volumetric_kit_gfx/runner-1 gfx
status=0
teardown recon || status=$?
expect "teardown succeeds" "$status" 0
[ "$status" -eq 0 ] || sed 's/^/     | /' "$tmp/out"
remaining=""
for dir in "$HOME"/ci-runners/*/runner-* "$HOME"/actions-runner-*; do
  [ -d "$dir" ] && remaining="$remaining ${dir#"$HOME"/}"
done
expect "it deletes the repository's runners and leftovers, and only them" "$remaining" \
  " ci-runners/volumetric_kit_gfx/runner-1 actions-runner-1"
expect "it deregisters each registered runner" \
  "$(calls '^config .* remove --token RMTOKEN$') $(calls '^config ci-runners/volumetric_kit_recon/runner-2')" "2 0"
expect "it uninstalls every service of theirs, a leftover's too" "$(calls ' uninstall$')" 3
expect "it checks gh once and mints one remove token" \
  "$(calls '^gh auth status$') $(calls remove-token)" "1 1"

for case in STUB_REMOVE_FAILS STUB_GH_UNAUTHED; do
  fresh_home
  runner ci-runners/volumetric_kit_recon/runner-1 recon
  status=0
  env "$case=1" "$BASH" "$tools/teardown-runners.sh" recon >"$tmp/out" 2>&1 || status=$?
  expect "teardown fails when it cannot deregister ($case)" "$status" 1
  if [ -f "$HOME/ci-runners/volumetric_kit_recon/runner-1/.runner" ]; then
    pass "it keeps the runner's credentials for a retry ($case)"
  else
    fail "it keeps the runner's credentials for a retry ($case)"
  fi
done
expect "without gh auth it leaves the runner running" \
  "$(calls ' uninstall$') $(running ci-runners/volumetric_kit_recon/runner-1)" "0 yes"

fresh_home
runner actions-runner-1 gfx
status=0
teardown recon || status=$?
expect "teardown with nothing to remove succeeds and touches nothing" \
  "$status $(calls '^(svc|config|gh) ')" "0 0"

export HOME="$real_home"
echo "$((count - failures)) of $count passed"
[ "$failures" -eq 0 ]
