#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
#
# Runs setup-runners.sh and teardown-runners.sh against a fake HOME, with
# stubs in place of the runner's config.sh and svc.sh, gh, curl, sudo and the
# host prerequisites, and checks what they did to which runner. No real runner
# or service is touched.
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
export CALLS="$tmp/calls.log"

# stub NAME BODY: an executable NAME on the stubs' PATH.
stub() {
  printf '#!/bin/sh\n%s\n' "$2" >"$stubs/$1"
  chmod +x "$stubs/$1"
}
stub sudo 'exec "$@"'
stub curl 'echo "curl $*" >>"$CALLS"; exit 1'
stub docker '[ "$1" = info ] && echo " Runtimes: nvidia runc"; exit 0'
stub nvidia-ctk 'exit 0'
stub xcode-select 'echo /Applications/Xcode.app/Contents/Developer'
stub xcodebuild 'echo Xcode'
stub brew 'exit 0'
stub gh '
echo "gh $*" >>"$CALLS"
case "$*" in
  "auth status") [ -z "${STUB_GH_UNAUTHED:-}" ] ;;
  *releases/latest*) echo v9.9.9 ;;
  *registration-token*) echo REGTOKEN ;;
  *remove-token*) echo RMTOKEN ;;
esac'

# The runner's own scripts, as the tarball ships them and config.sh leaves
# them. Registering writes .runner as the real one does.
printf '%s\n' '#!/bin/sh' \
  'echo "config ${PWD#"$HOME"/} $*" >>"$CALLS"' \
  'if [ "$1" = remove ]; then' \
  '  [ -z "${STUB_REMOVE_FAILS:-}" ] || exit 1' \
  '  rm -f .runner; exit 0' \
  'fi' \
  'while [ $# -gt 0 ]; do [ "$1" = --url ] && url="$2"; shift; done' \
  'printf "{\n  \"gitHubUrl\": \"%s\"\n}\n" "$url" >.runner' >"$runner_files/config.sh"
printf '%s\n' '#!/bin/sh' 'echo "svc ${PWD#"$HOME"/} $*" >>"$CALLS"' >"$runner_files/svc.sh"
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

# runner DIR REPO [ENV]: a runner directory registered to REPO (none if
# empty), with ENV as its .env (none if absent).
runner() {
  mkdir -p "$HOME/$1"
  cp "$runner_files/config.sh" "$runner_files/svc.sh" "$HOME/$1/"
  if [ -n "$2" ]; then
    printf '{\n  "gitHubUrl": "https://github.com/taojin-6/volumetric_kit_%s"\n}\n' \
      "$2" >"$HOME/$1/.runner"
  fi
  if [ $# -ge 3 ]; then printf '%b' "$3" >"$HOME/$1/.env"; fi
}

calls() { grep -c -E -e "$1" "$CALLS" || true; }
env_of() { cat "$HOME/$1/.env" 2>/dev/null || echo "<none>"; }

setup() { bash "$tools/setup-runners.sh" "$@" >"$tmp/out" 2>&1; }
teardown() { bash "$tools/teardown-runners.sh" "$@" >"$tmp/out" 2>&1; }

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
# cache_tarball: a cached runner tarball for the version gh reports, so
# nothing downloads.
mkdir -p "$tmp/tarball"
cp "$runner_files/config.sh" "$runner_files/svc.sh" "$tmp/tarball/"
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
cache_tarball
status=0
setup recon || status=$?
expect "registration succeeds" "$status" 0
[ "$status" -eq 0 ] || sed 's/^/     | /' "$tmp/out"
expect "one registration token is minted" "$(calls registration-token)" 1
expect "nothing is downloaded when the tarball is cached" "$(calls '^curl')" 0
expect "a registered runner is not registered again" \
  "$(calls '^config ci-runners/volumetric_kit_recon/runner-1 ')" 0
host="$(hostname -s)"
registered=0
for i in $(seq 2 "$n"); do
  grep -qF "config ci-runners/volumetric_kit_recon/runner-$i --unattended --url https://github.com/taojin-6/volumetric_kit_recon --token REGTOKEN --labels $label --name $host-recon-$i " "$CALLS" &&
    registered=$((registered + 1))
done
expect "the missing runners are registered, named and labelled" "$registered" "$((n - 1))"
expect "every runner's service is installed" "$(calls "^svc ci-runners/volumetric_kit_recon/runner-[0-9]* $install\$")" "$n"
threads=$((cores / (n + 1)))
[ "$threads" -ge 1 ] || threads=1
expect "the cores are split across every repository's runners; an old level is replaced in place, once, and other lines kept" \
  "$(env_of ci-runners/volumetric_kit_recon/runner-1)" "CTEST_PARALLEL_LEVEL=$threads
LANG=C.UTF-8
CMAKE_BUILD_PARALLEL_LEVEL=$threads"
expect "a new runner gets the same levels" \
  "$(env_of "ci-runners/volumetric_kit_recon/runner-$n")" "CMAKE_BUILD_PARALLEL_LEVEL=$threads
CTEST_PARALLEL_LEVEL=$threads"
expect "each runner whose .env changed restarts" \
  "$(calls '^svc ci-runners/volumetric_kit_recon/runner-[0-9]* start$')" "$n"
expect "only the repository's runners restart" "$(calls '^svc .*gfx')" 0
expect "another repository's runner is untouched" \
  "$(env_of ci-runners/volumetric_kit_gfx/runner-1)" "LANG=C"

: >"$CALLS"
status=0
setup recon || status=$?
expect "a re-run succeeds" "$status" 0
expect "a re-run asks gh for nothing and registers nothing" "$(calls '^(config|gh|curl) ')" 0
expect "a re-run restarts nothing" "$(calls ' stop$')" 0

# A host whose runners are still in the pre-layout directories gets no second
# set, and its thread split counts them, every repository's.
fresh_home
for i in $(seq 1 "$n"); do runner "actions-runner-recon-$i" recon; done
runner actions-runner-1 gfx 'LANG=C\n'
status=0
setup recon || status=$?
expect "a full run on a pre-layout host succeeds" "$status" 0
[ "$status" -eq 0 ] || sed 's/^/     | /' "$tmp/out"
expect "it asks gh for nothing and registers nothing" "$(calls '^(config|gh|curl) ')" 0
if [ -e "$HOME/ci-runners/volumetric_kit_recon" ]; then
  fail "it creates no runner beside them"
else
  pass "it creates no runner beside them"
fi
threads=$((cores / (n + 1)))
[ "$threads" -ge 1 ] || threads=1
expect "they get a split across every pre-layout runner" \
  "$(env_of "actions-runner-recon-$n")" "CMAKE_BUILD_PARALLEL_LEVEL=$threads
CTEST_PARALLEL_LEVEL=$threads"
expect "another repository's pre-layout runner is untouched" "$(env_of actions-runner-1)" "LANG=C"

fresh_home
for i in $(seq 2 "$n"); do runner "actions-runner-recon-$i" recon; done
cache_tarball
status=0
setup recon || status=$?
expect "a pre-layout host short of runners succeeds" "$status" 0
[ "$status" -eq 0 ] || sed 's/^/     | /' "$tmp/out"
expect "it registers only the missing one" \
  "$(calls '^config ci-runners/volumetric_kit_recon/runner-1 ') $(calls '^config ')" "1 1"

# --- Teardown ---------------------------------------------------------------

fresh_home
runner ci-runners/volumetric_kit_recon/runner-1 recon
runner ci-runners/volumetric_kit_recon/runner-2 ""
runner actions-runner-recon-3 recon
runner actions-runner-1 gfx
runner ci-runners/volumetric_kit_gfx/runner-1 gfx
status=0
teardown recon || status=$?
expect "teardown succeeds" "$status" 0
remaining=""
for dir in "$HOME"/ci-runners/*/runner-* "$HOME"/actions-runner-*; do
  [ -d "$dir" ] && remaining="$remaining ${dir#"$HOME"/}"
done
expect "it deletes the repository's runners and only them" "$remaining" \
  " ci-runners/volumetric_kit_gfx/runner-1 actions-runner-1"
expect "it deregisters each registered runner" \
  "$(calls '^config .* remove --token RMTOKEN$') $(calls '^config ci-runners/volumetric_kit_recon/runner-2')" "2 0"
expect "it stops and uninstalls their services" "$(calls ' stop$') $(calls ' uninstall$')" "3 3"

for case in STUB_REMOVE_FAILS STUB_GH_UNAUTHED; do
  fresh_home
  runner ci-runners/volumetric_kit_recon/runner-1 recon
  status=0
  env "$case=1" bash "$tools/teardown-runners.sh" recon >"$tmp/out" 2>&1 || status=$?
  expect "teardown fails when it cannot deregister ($case)" "$status" 1
  if [ -f "$HOME/ci-runners/volumetric_kit_recon/runner-1/.runner" ]; then
    pass "it keeps the runner's credentials for a retry ($case)"
  else
    fail "it keeps the runner's credentials for a retry ($case)"
  fi
done

export HOME="$real_home"
echo "$((count - failures)) of $count passed"
[ "$failures" -eq 0 ]
