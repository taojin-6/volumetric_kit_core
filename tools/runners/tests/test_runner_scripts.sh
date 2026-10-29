#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
#
# Runs setup-runners.sh and teardown-runners.sh against a fake HOME, with
# stubs in place of the runner's config.sh and svc.sh, launchctl, gh, curl,
# sudo and the host prerequisites, and checks what they did to which runner.
# No real runner or service is touched: macOS's daemons go to a scratch
# directory (RUNNER_DAEMON_DIR). The host has $STUB_CORES cores (default 24),
# and with STUB_OS set, `uname -s` names that system instead of this one's, so
# the cases run as Linux and as macOS on any host. The scripts run under the
# bash running this, so `/bin/bash test_runner_scripts.sh` tests them under
# macOS's bash 3.2.
# shellcheck disable=SC2016 # the stubs' variables expand when the stubs run
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tools="$here/.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
# shellcheck source-path=SCRIPTDIR source=lib.sh
. "$here/lib.sh"

# --- Stubs ------------------------------------------------------------------

stubs="$tmp/stubs"
runner_files="$tmp/runner-files"
mkdir -p "$stubs" "$runner_files"
export CALLS="$tmp/calls.log" RUNNER_FILES="$runner_files"
export RUNNER_DAEMON_DIR="$tmp/daemons" STUB_HOMES="$tmp/homes"

# stub NAME BODY: an executable NAME on the stubs' PATH.
stub() {
  printf '#!/bin/sh\n%s\n' "$2" >"$stubs/$1"
  chmod +x "$stubs/$1"
}
# As root; with -u ACCOUNT -H, as an account whose home is $STUB_HOMES/ACCOUNT,
# which the real user stands in for, and which cannot read $STUB_CLOSED.
stub sudo 'if [ "$1" = -u ]; then
  echo "sudo -u $2" >>"$CALLS"
  [ -d "$STUB_HOMES/$2" ] || { echo "sudo: unknown user $2" >&2; exit 1; }
  case " $* " in *" ${STUB_CLOSED:-/nonexistent}"*) echo "Permission denied" >&2; exit 1 ;; esac
  HOME="$STUB_HOMES/$2" STUB_AS="$2"
  export HOME STUB_AS
  shift 2
  [ "$1" != -H ] || shift
  exec "$@"
fi
STUB_ROOT=1 exec "$@"'
# An account's home is closed to this user: only the account or root deletes
# in it.
stub rm 'case " $* " in *" $STUB_HOMES/"*)
  [ -n "${STUB_AS:-}${STUB_ROOT:-}" ] || { echo "rm: Permission denied" >&2; exit 1; } ;;
esac
exec /bin/rm "$@"'
stub uname 'if [ "$1" = -s ] && [ -n "${STUB_OS:-}" ]; then echo "$STUB_OS"; else exec '"$(command -v uname)"' "$@"; fi'
stub nproc 'echo "${STUB_CORES:-24}"'
stub sysctl '[ "$*" = "-n hw.ncpu" ] && echo "${STUB_CORES:-24}"'
stub sleep 'exit 0' # launchd's state changes below take calls, not time
# Only root writes a daemon, and STUB_INSTALL_FAILS makes the write fail.
stub cp 'for last; do :; done
case "$last" in
  "$RUNNER_DAEMON_DIR"/*)
    [ -n "${STUB_ROOT:-}" ] || { echo "cp: $last: Permission denied" >&2; exit 1; }
    [ -z "${STUB_INSTALL_FAILS:-}" ] || exit 1 ;;
esac
exec '"$(command -v cp)"' "$@"'
# macOS's own where it exists; elsewhere a property list is one that ends.
stub plutil 'if [ -x /usr/bin/plutil ]; then exec /usr/bin/plutil "$@"; fi
[ "$1" = -lint ] && grep -q "</plist>" "$2"'
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
# gh mints tokens, and answers a GET with $STUB_API/repo<rest of the path>,
# the path past repos/<owner>/<repo>, its query dropped and each / an _,
# through jq in place of its own --jq; a path with no file is not found.
stub gh '
{ printf "gh %s" "$*" | tr "\n" " "; echo; } >>"$CALLS"
case "$*" in
  "auth status") [ -z "${STUB_GH_UNAUTHED:-}" ]; exit ;;
  *registration-token*) echo REGTOKEN; exit ;;
  *remove-token*) echo RMTOKEN; exit ;;
esac
shift
path="" filter=.
while [ $# -gt 0 ]; do
  case "$1" in
    --jq) filter="$2"; shift ;;
    --paginate) ;;
    *) path="${1%%\?*}" ;;
  esac
  shift
done
f="$STUB_API/repo$(echo "$path" | sed "s|^repos/[^/]*/[^/]*||" | tr / _)"
[ -f "$f" ] || { echo "gh: Not Found (HTTP 404)" >&2; exit 1; }
jq -r "$filter" "$f"'
command -v jq >/dev/null 2>&1 || {
  echo "these tests need jq, which stands in for gh's --jq" >&2
  exit 1
}

# api SETTINGS: what the stub's GitHub says of every repository: the owner's
# (ID 22780239), with one active policy over every workflow that lets only the
# owner trigger them, and, "public", approval of every outside contributor's
# run, or "private", fork pull requests' workflows off. A case edits the files
# after this.
export STUB_API="$tmp/api"
owner_actor='{"id": 22780239, "type": "User"}'
all_workflows='{"workflow_path": {"include": ["~ALL"], "exclude": []}}'
# policy ID ENFORCEMENT ACTORS [CONDITIONS]: a policy as GitHub returns one by
# ID, whose one rule lets ACTORS, a JSON list's items, trigger the workflows
# CONDITIONS (default every one) select.
policy() {
  printf '{"id": %s, "name": "Policy %s", "enforcement": "%s", "conditions": %s, "rules": [{"type": "restrict_actions_actors", "parameters": {"allowed_actors": [%s]}}]}\n' \
    "$1" "$1" "$2" "${4:-$all_workflows}" "$3"
}
api() {
  rm -rf "$STUB_API"
  mkdir -p "$STUB_API"
  echo '{"total_count": 1, "policies": [{"id": 7054}]}' >"$STUB_API/repo_actions_policies"
  policy 7054 active "$owner_actor" >"$STUB_API/repo_actions_policies_7054"
  if [ "$1" = private ]; then
    echo '{"private": true, "owner": {"id": 22780239, "login": "taojin-6"}}' >"$STUB_API/repo"
    echo '{"run_workflows_from_fork_pull_requests": false}' \
      >"$STUB_API/repo_actions_permissions_fork-pr-workflows-private-repos"
    return
  fi
  echo '{"private": false, "owner": {"id": 22780239, "login": "taojin-6"}}' >"$STUB_API/repo"
  echo '{"approval_policy": "all_external_contributors"}' \
    >"$STUB_API/repo_actions_permissions_fork-pr-contributor-approval"
}
api public

# launchd. A daemon (system/LABEL) keeps its state in the runner directory its
# plist names: .loaded once bootstrapped, .running while it runs; only root
# changes it, and $RUNNER_DAEMON_DIR/LABEL.disabled refuses a bootstrap until
# an enable. A LaunchAgent (gui/UID/LABEL) keeps LABEL.loaded in
# $HOME/.stub-agents. As launchd does, it refuses to bootstrap a loaded
# service, and a bootout returns before the service is gone: it stays loaded,
# and running, for two more prints. STUB_BOOTOUT_FAILS makes a daemon's fail.
# That state is launchd's own, which the rm stub below does not guard.
stub launchctl '
echo "launchctl $*" >>"$CALLS"
kill=""
if [ "$1" = kickstart ] && [ "$2" = -k ]; then
  kill=1
  shift 2
  set -- kickstart "$@"
fi
d=""
case "$2" in
  system | system/*)
    if [ "$2" = system ]; then label="$(basename "$3" .plist)"; else label="${2#system/}"; fi
    d="$(sed -n "s|.*<key>WorkingDirectory</key><string>\(.*\)</string>.*|\1|p" \
      "$RUNNER_DAEMON_DIR/$label.plist" 2>/dev/null)"
    p="${d:-/nonexistent}/."
    [ "$1" = print ] || [ -n "${STUB_ROOT:-}" ] || { echo "Operation not permitted" >&2; exit 1; } ;;
  gui/*)
    label="${2##*/}"
    p="$HOME/.stub-agents/$label." ;;
  *) exit 0 ;;
esac
loaded() { [ -f "${p}loaded" ]; }
case "$1" in
  print)
    if [ -f "${p}unloading" ]; then
      left="$(cat "${p}unloading")"
      if [ "$left" -gt 0 ]; then echo $((left - 1)) >"${p}unloading"; else /bin/rm -f "${p}loaded" "${p}running" "${p}unloading"; fi
    fi
    loaded || { echo "Could not find service" >&2; exit 113; }
    if [ -f "${p}running" ]; then echo "	state = running"; else echo "	state = not running"; fi ;;
  enable) rm -f "$RUNNER_DAEMON_DIR/$label.disabled" ;;
  bootstrap)
    [ ! -f "$RUNNER_DAEMON_DIR/$label.disabled" ] || { echo "Bootstrap failed: 119: Service is disabled" >&2; exit 119; }
    [ -n "$d" ] && ! loaded || { echo "Bootstrap failed: 5: Input/output error" >&2; exit 5; }
    touch "${p}loaded"
    [ -n "${STUB_START_FAILS:-}" ] || touch "${p}running" ;;
  kickstart)
    loaded && [ ! -f "${p}unloading" ] || exit 113
    [ -z "$kill" ] || /bin/rm -f "${p}running"
    [ -n "${STUB_START_FAILS:-}" ] || touch "${p}running" ;;
  bootout)
    loaded || exit 113
    [ -z "$d" ] || [ -z "${STUB_BOOTOUT_FAILS:-}" ] || exit 5
    echo 2 >"${p}unloading" ;;
esac
exit 0'

# The runner's own scripts. Registering writes .runner and generates svc.sh,
# naming the runner's service, as the real config.sh does, and deregistering
# is refused while .service exists. svc.sh keeps the service's state in
# .service (installed, as the real one does) and .running, and prints its
# status the way the real one does on Linux.
printf '%s\n' '#!/bin/sh' \
  'echo "config ${PWD#"$HOME"/} $*" >>"$CALLS"' \
  'if [ "$1" = remove ]; then' \
  '  [ -z "${STUB_REMOVE_FAILS:-}" ] || exit 1' \
  '  [ ! -f .service ] || { echo "Uninstall service first" >&2; exit 1; }' \
  '  rm -f .runner; exit 0' \
  'fi' \
  'while [ $# -gt 0 ]; do' \
  '  case "$1" in --url) url="$2" ;; --name) name="$2" ;; esac' \
  '  shift' \
  'done' \
  'printf "{\n  \"gitHubUrl\": \"%s\"\n}\n" "$url" >.runner' \
  'owner_repo="$(echo "${url#https://github.com/}" | tr / -)"' \
  '{ echo "#!/bin/sh"; echo "SVC_NAME=\"actions.runner.$owner_repo.$name\""; tail -n +2 "$RUNNER_FILES/svc.sh"; } >svc.sh' \
  'chmod +x svc.sh' >"$runner_files/config.sh"
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
printf '#!/bin/sh\n' >"$runner_files/runsvc.sh"
chmod +x "$runner_files/config.sh" "$runner_files/svc.sh" "$runner_files/runsvc.sh"

export PATH="$stubs:$PATH"
# setup-runners.sh writes the shell's MoltenVK_ROOT into each .env; only the
# cases below that set it expect it.
unset MoltenVK_ROOT
real_home="$HOME"
user="${USER:-$(id -un)}"
export USER="$user"

# fresh_home: an empty HOME, daemon directory and calls log, and no other
# account.
fresh_home() {
  export HOME="$tmp/home"
  rm -rf "$HOME" "$RUNNER_DAEMON_DIR" "$STUB_HOMES"
  mkdir -p "$HOME" "$RUNNER_DAEMON_DIR" "$STUB_HOMES"
  : >"$CALLS"
}

# runner DIR REPO [ENV]: a runner directory registered to REPO, a short name
# or owner/repo, its service installed and running, as setup-runners.sh leaves
# one -- for Linux and for macOS alike, so it serves either; with REPO empty,
# one that never finished registering. ENV is its .env (none if absent).
runner() {
  local d="$HOME/$1" repo="$2" label
  case "$repo" in */*) ;; ?*) repo="taojin-6/volumetric_kit_$repo" ;; esac
  mkdir -p "$d/bin"
  cp "$runner_files/config.sh" "$d/"
  cp "$runner_files/runsvc.sh" "$d/bin/"
  if [ -n "$repo" ]; then
    label="actions.runner.$(echo "$repo" | tr / -).$(basename "$1")"
    printf '{\n  "gitHubUrl": "https://github.com/%s"\n}\n' "$repo" >"$d/.runner"
    { echo '#!/bin/sh'; echo "SVC_NAME=\"$label\""; tail -n +2 "$runner_files/svc.sh"; } >"$d/svc.sh"
    chmod +x "$d/svc.sh"
    echo unit >"$d/.service"
    touch "$d/.running" "$d/.loaded"
    printf '<key>WorkingDirectory</key><string>%s</string>\n' "$d" \
      >"$RUNNER_DAEMON_DIR/$label.plist"
  fi
  if [ $# -ge 3 ]; then printf '%b' "$3" >"$d/.env"; fi
}

label_of() { sed -n 's/^SVC_NAME="\(.*\)"$/\1/p' "$HOME/$1/svc.sh"; }
daemon_of() { echo "$RUNNER_DAEMON_DIR/$(label_of "$1").plist"; }
# pause DIR: its service stopped, as README's pause leaves it.
pause() { rm -f "$HOME/$1/.running" "$HOME/$1/.loaded" "$HOME/$1/.unloading"; }
# unservice DIR: its service never installed, or lost.
unservice() {
  pause "$1"
  rm -f "$(daemon_of "$1")" "$HOME/$1/.service"
}
# agent DIR [none]: a LaunchAgent of runner DIR loaded in the login session, as
# an earlier setup left it; with "none", its plist already deleted.
agent_plist() { echo "$HOME/Library/LaunchAgents/$(label_of "$1").plist"; }
agent_mark() { echo "$HOME/.stub-agents/$(label_of "$1").loaded"; }
agent() {
  mkdir -p "$HOME/Library/LaunchAgents" "$HOME/.stub-agents"
  [ "${2:-}" = none ] || echo agent >"$(agent_plist "$1")"
  touch "$(agent_mark "$1")"
}
# agent_state DIR: whether its agent is still loaded, and its plist still there.
agent_state() {
  local loaded=unloaded plist=deleted
  [ ! -f "$(agent_mark "$1")" ] || loaded=loaded
  [ ! -f "$(agent_plist "$1")" ] || plist=kept
  echo "$loaded $plist"
}

calls() { grep -c -E -e "$1" "$CALLS" || true; }
env_of() { cat "$HOME/$1/.env" 2>/dev/null || echo "<none>"; }
# running DIR: whether its service runs: on macOS only while launchd has it.
running() {
  if [ -f "$HOME/$1/.running" ] && { [ "$os" = Linux ] || [ -f "$HOME/$1/.loaded" ]; }; then
    echo yes
  else
    echo no
  fi
}
levels() { printf 'CMAKE_BUILD_PARALLEL_LEVEL=%s\nCTEST_PARALLEL_LEVEL=%s' "$1" "$1"; }
# daemons: the installed daemons' file names, each followed by a space.
daemons() {
  local f
  for f in "$RUNNER_DAEMON_DIR"/*.plist; do
    if [ -f "$f" ]; then printf '%s ' "${f##*/}"; fi
  done
}

# The service as $os sees it: its .service on Linux, its daemon on macOS.
installed() {
  if { [ "$os" = Linux ] && [ -f "$HOME/$1/.service" ]; } ||
    { [ "$os" = Darwin ] && [ -f "$(daemon_of "$1")" ]; }; then
    echo yes
  else
    echo no
  fi
}
# restarts DIR: how often its service was restarted -- svc.sh's stop and
# start on Linux, a kickstart -k on macOS.
restarts() {
  if [ "$os" = Linux ]; then
    calls "^svc $1 stop\$"
  else
    calls "^launchctl kickstart -k system/$(label_of "$1")\$"
  fi
}
# changes: the services installed, started, stopped or uninstalled.
changes() {
  if [ "$os" = Linux ]; then
    calls '^svc [^ ]* (install|start|stop|uninstall)( |$)'
  else
    calls '^launchctl (bootstrap|bootout|kickstart|enable) '
  fi
}
# touched REPOS: the service calls, reading their state included, on the
# runners of REPOS, an alternation.
touched() {
  if [ "$os" = Linux ]; then
    calls "^svc [^ ]*volumetric_kit_($1)/"
  else
    calls "^launchctl .*volumetric_kit_($1)\."
  fi
}

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
status=0
teardown core || status=$?
expect "teardown-runners.sh rejects 'core', naming the repositories it takes" \
  "$status $(grep -c "unknown repository 'core': expected recon, gfx, calib, player or owner/repo" "$tmp/out" || true)" \
  "2 1"
# Any other repository is named in full, in GitHub's characters, and its name
# is not dots alone, which would put its runners outside ~/ci-runners: there
# teardown would take these directories for leftovers and delete them. Neither
# script touches anything for a repository it refuses.
mkdir -p "$HOME/runner-1" "$HOME/ci-runners/runner-1"
for repo in owner/ /repo owner/repo/extra owner/. owner/.. ../repo own.er/repo 'owner/re po' 'owner/re$po'; do
  status=0
  setup "$repo" || status=$?
  expect "setup-runners.sh rejects '$repo'" "$status $(calls '^(config|svc|gh|curl|launchctl) ')" "2 0"
  status=0
  teardown "$repo" || status=$?
  expect "teardown-runners.sh rejects '$repo' and deletes nothing" \
    "$status $(calls '^(config|svc|gh|curl|launchctl) ') $([ -d "$HOME/runner-1" ] && [ -d "$HOME/ci-runners/runner-1" ] && echo kept)" \
    "2 0 kept"
done
# No repository has runners on a system but Linux and macOS; the refusal
# names those.
status=0
STUB_OS=FreeBSD setup recon || status=$?
expect "setup-runners.sh refuses a system with no runners, naming those that have them" \
  "$status $(calls '^(config|gh|curl) ') $(grep -c 'volumetric_kit_recon has no self-hosted runners on FreeBSD, only on Linux and Darwin ' "$tmp/out" || true)" \
  "2 0 1"

# --- Registration -----------------------------------------------------------

cores=24
host="$(hostname -s)"
# share RUNNERS: the build level with the cores split across RUNNERS.
share() {
  local threads=$((cores / $1))
  [ "$threads" -ge 1 ] || threads=1
  echo "$threads"
}
# cache_tarball: a cached runner tarball for the version the release page
# redirects to, so nothing downloads. Like the real one, it has no svc.sh.
mkdir -p "$tmp/tarball/bin"
cp "$runner_files/config.sh" "$tmp/tarball/"
cp "$runner_files/runsvc.sh" "$tmp/tarball/bin/"
cache_tarball() {
  local os arch
  mkdir -p "$HOME/ci-runners"
  for os in linux osx; do
    for arch in x64 arm64; do
      tar czf "$HOME/ci-runners/actions-runner-$os-$arch-9.9.9.tar.gz" -C "$tmp/tarball" .
    done
  done
}

for os in Linux Darwin; do
  export STUB_OS="$os"
  if [ "$os" = Linux ]; then n=6 label=vk-linux-gpu; else n=2 label=mac; fi

  fresh_home
  runner ci-runners/volumetric_kit_recon/runner-1 recon \
    'CTEST_PARALLEL_LEVEL=999\nLANG=C.UTF-8\nCTEST_PARALLEL_LEVEL=999\n'
  runner ci-runners/volumetric_kit_gfx/runner-1 gfx 'LANG=C\n'
  runner ci-runners/volumetric_kit_gfx/runner-2 gfx 'LANG=C\n'
  pause ci-runners/volumetric_kit_gfx/runner-2
  cache_tarball
  status=0
  setup recon || status=$?
  expect "registration succeeds ($os)" "$status" 0
  [ "$status" -eq 0 ] || sed 's/^/     | /' "$tmp/out"
  expect "one registration token is minted ($os)" "$(calls registration-token)" 1
  expect "nothing is downloaded when the tarball is cached ($os)" "$(calls '^curl .*/download/')" 0
  expect "a registered runner is not registered again ($os)" \
    "$(calls '^config ci-runners/volumetric_kit_recon/runner-1 ')" 0
  registered=0 services=0 up=0
  for i in $(seq 1 "$n"); do
    d="ci-runners/volumetric_kit_recon/runner-$i"
    [ "$i" -eq 1 ] ||
      ! grep -qF "config $d --unattended --url https://github.com/taojin-6/volumetric_kit_recon --token REGTOKEN --labels $label --name $host-recon-$i " "$CALLS" ||
      registered=$((registered + 1))
    [ "$(installed "$d")" = no ] || services=$((services + 1))
    [ "$(running "$d")" = no ] || up=$((up + 1))
  done
  expect "the missing runners are registered, named and labelled ($os)" "$registered" "$((n - 1))"
  expect "every runner of the repository has its service installed, and runs ($os)" \
    "$services $up" "$n $n"
  if [ "$os" = Linux ]; then
    expect "each new runner's service is installed as this user (Linux)" \
      "$(calls "^svc ci-runners/volumetric_kit_recon/runner-[0-9]* install $user\$")" "$((n - 1))"
  else
    d=ci-runners/volumetric_kit_recon/runner-2
    daemon="$(daemon_of "$d")"
    expect "a new runner's daemon runs its runsvc.sh as this user, kept alive, with HOME (Darwin)" \
      "$(grep -c -F -e "<key>Label</key><string>$(label_of "$d")</string>" \
        -e "<string>$HOME/$d/runsvc.sh</string>" -e "<key>UserName</key><string>$user</string>" \
        -e '<key>KeepAlive</key><true/>' -e "<key>HOME</key><string>$HOME</string>" "$daemon")" 5
    expect "its runsvc.sh is the runner's, executable, and its .service names the daemon (Darwin)" \
      "$([ -x "$HOME/$d/runsvc.sh" ] && echo executable) $(cat "$HOME/$d/.service")" \
      "executable $daemon"
    if command -v plutil >/dev/null 2>&1; then
      expect "the daemon is a valid property list (Darwin)" \
        "$(plutil -lint "$daemon" >/dev/null 2>&1 && echo valid)" valid
    fi
  fi
  threads="$(share "$n")"
  expect "the cores are split across the repository's runners alone; an old level is replaced in place, once, and other lines kept ($os)" \
    "$(env_of ci-runners/volumetric_kit_recon/runner-1)" "CTEST_PARALLEL_LEVEL=$threads
LANG=C.UTF-8
CMAKE_BUILD_PARALLEL_LEVEL=$threads"
  expect "a new runner gets the same levels ($os)" \
    "$(env_of "ci-runners/volumetric_kit_recon/runner-$n")" "$(levels "$threads")"
  expect "a running runner whose .env changed restarts ($os)" \
    "$(restarts ci-runners/volumetric_kit_recon/runner-1)" 1
  [ "$os" = Linux ] ||
    expect "a daemon restarts in place, with nothing unloaded (Darwin)" \
      "$(calls '^launchctl bootout ')" 0
  expect "another repository's runners keep their .env ($os)" \
    "$(env_of ci-runners/volumetric_kit_gfx/runner-1) $(env_of ci-runners/volumetric_kit_gfx/runner-2)" \
    "LANG=C LANG=C"
  expect "another repository's runners are not restarted, and its paused one stays paused ($os)" \
    "$(touched gfx) $(running ci-runners/volumetric_kit_gfx/runner-1) $(running ci-runners/volumetric_kit_gfx/runner-2)" \
    "0 yes no"

  : >"$CALLS"
  status=0
  setup recon || status=$?
  expect "a re-run succeeds ($os)" "$status" 0
  expect "a re-run asks gh for nothing and registers nothing ($os)" "$(calls '^(config|gh|curl) ')" 0
  expect "a re-run installs, stops and starts nothing ($os)" "$(changes)" 0

  # A paused runner of the repository is started by a re-run; on macOS so is
  # a loaded daemon that is not running, kicked rather than loaded again.
  pause ci-runners/volumetric_kit_recon/runner-1
  status=0
  setup recon || status=$?
  expect "a re-run starts the repository's paused runner ($os)" \
    "$status $(running ci-runners/volumetric_kit_recon/runner-1)" "0 yes"
  if [ "$os" = Darwin ]; then
    rm "$HOME/ci-runners/volumetric_kit_recon/runner-1/.running"
    : >"$CALLS"
    status=0
    setup recon || status=$?
    expect "a loaded daemon that is not running is kicked, not loaded again (Darwin)" \
      "$status $(running ci-runners/volumetric_kit_recon/runner-1) $(calls '^launchctl kickstart system/') $(calls '^launchctl bootstrap ')" \
      "0 yes 1 0"
    pause ci-runners/volumetric_kit_recon/runner-1
    disabled="$RUNNER_DAEMON_DIR/$(label_of ci-runners/volumetric_kit_recon/runner-1).disabled"
    touch "$disabled"
    status=0
    setup recon || status=$?
    expect "a disabled daemon is enabled and started (Darwin)" \
      "$status $(running ci-runners/volumetric_kit_recon/runner-1) $([ -f "$disabled" ] && echo disabled || echo enabled)" \
      "0 yes enabled"
  fi

  # README's pause stops the repository's runners; on macOS it waits until
  # launchd lets go of each, so a re-run straight after can load it again.
  status=0
  "$BASH" -c '. "$1/common.sh"; select_repo recon; find_runner_dirs
    for d in "${RUNNER_DIRS[@]}"; do stop_service "$d"; done' _ "$tools" >"$tmp/out" 2>&1 ||
    status=$?
  expect "README's pause stops the repository's runners ($os)" \
    "$status $(running ci-runners/volumetric_kit_recon/runner-1) $(running "ci-runners/volumetric_kit_recon/runner-$n") $(running ci-runners/volumetric_kit_gfx/runner-1)" \
    "0 no no yes"
  status=0
  setup recon || status=$?
  expect "a re-run straight after starts them again ($os)" \
    "$status $(running ci-runners/volumetric_kit_recon/runner-1) $(running "ci-runners/volumetric_kit_recon/runner-$n")" \
    "0 yes yes"

  # A service that does not install or start fails the run, which still sets
  # up the rest.
  for case in STUB_INSTALL_FAILS STUB_START_FAILS; do
    fresh_home
    for i in $(seq 1 "$n"); do runner "ci-runners/volumetric_kit_recon/runner-$i" recon; done
    unservice ci-runners/volumetric_kit_recon/runner-1
    status=0
    env "$case=1" "$BASH" "$tools/setup-runners.sh" recon >"$tmp/out" 2>&1 || status=$?
    expect "setup fails when a service does not come up ($case, $os)" "$status" 1
    expect "it still sets up the other runners ($case, $os)" \
      "$(env_of "ci-runners/volumetric_kit_recon/runner-$n")" "$(levels "$(share "$n")")"
    [ "$case" != STUB_INSTALL_FAILS ] ||
      expect "a failed install leaves no service behind ($os)" \
        "$(installed ci-runners/volumetric_kit_recon/runner-1)" no
  done

  # A host whose runners are still in the pre-layout directories gets no
  # second set, and its thread split counts them, but no other repository's.
  # One whose service is gone is reinstalled, and an unregistered leftover is
  # left alone.
  fresh_home
  for i in $(seq 1 "$n"); do runner "actions-runner-recon-$i" recon; done
  unservice actions-runner-recon-1
  runner actions-runner-1 gfx 'LANG=C\n'
  runner ci-runners/volumetric_kit_recon/runner-1 ""
  status=0
  setup recon || status=$?
  expect "a full run on a pre-layout host succeeds ($os)" "$status" 0
  [ "$status" -eq 0 ] || sed 's/^/     | /' "$tmp/out"
  expect "it asks gh for nothing and registers nothing ($os)" "$(calls '^(config|gh|curl) ')" 0
  expect "it counts only registered runners ($os)" \
    "$(grep -c "^Done: $n runners " "$tmp/out" || true)" 1
  expect "it leaves an unregistered leftover's .env alone ($os)" \
    "$(env_of ci-runners/volumetric_kit_recon/runner-1)" "<none>"
  [ "$os" = Darwin ] || # a leftover names no daemon
    expect "it leaves an unregistered leftover's service alone (Linux)" \
      "$(calls '^svc ci-runners/')" 0
  expect "it reinstalls and starts a pre-layout runner's service ($os)" \
    "$(installed actions-runner-recon-1) $(running actions-runner-recon-1)" "yes yes"
  expect "they get the split across the repository's pre-layout runners ($os)" \
    "$(env_of "actions-runner-recon-$n")" "$(levels "$(share "$n")")"
  expect "another repository's pre-layout runner keeps its .env ($os)" \
    "$(env_of actions-runner-1)" "LANG=C"

  fresh_home
  for i in $(seq 2 "$n"); do runner "actions-runner-recon-$i" recon; done
  cache_tarball
  status=0
  setup recon || status=$?
  expect "a pre-layout host short of runners succeeds ($os)" "$status" 0
  [ "$status" -eq 0 ] || sed 's/^/     | /' "$tmp/out"
  expect "it registers only the missing one ($os)" \
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
  expect "a pre-layout host short of a runner other than the first succeeds ($os)" "$status" 0
  [ "$status" -eq 0 ] || sed 's/^/     | /' "$tmp/out"
  expect "it names the new runner after a free number, not a working runner's ($os)" \
    "$(calls "^config ci-runners/volumetric_kit_recon/runner-2 .* --name $host-recon-2 ") $(calls '^config ')" "1 1"
done

# --- Who can trigger the workflows -------------------------------------------

# Before it registers a runner, setup reads who can trigger the repository's
# workflows, and unless only the owner can, it refuses, saying what to set and
# how, before it registers anything, mints a token or fetches the runner. Any
# repository needs an active policy over every workflow that lets only the
# owner trigger them, and none that lets another actor; a public one also
# approval of every outside contributor's run, a private one fork pull
# requests' workflows off. The checks are the same on every system.
export STUB_OS=Linux os=Linux
# gate NAME WANT [TEXT...]: setup of recon's runners, with the stub's GitHub
# as the caller left it, registers them (WANT ok), or is refused before
# anything (WANT refused) with every fixed string TEXT in its output.
gate() {
  local name="$1" want="$2" status=0 got text
  shift 2
  fresh_home
  cache_tarball
  setup recon || status=$?
  if [ "$status" -eq 0 ] && [ "$(calls '^config ')" -eq 6 ]; then
    got=ok
  elif [ "$status" -eq 1 ] && [ "$(calls '^(config|curl) |registration-token')" -eq 0 ]; then
    got=refused
    for text; do grep -qF -e "$text" "$tmp/out" || got="refused, without '$text'"; done
  else
    got="status $status, $(calls '^config ') registered"
  fi
  expect "$name" "$got" "$want"
  [ "$got" = "$want" ] || sed 's/^/     | /' "$tmp/out"
}
policies() { # policies [ID...]: the repository's policies, by ID
  local ids=""
  [ $# -eq 0 ] || ids="$(printf '{"id": %s}, ' "$@")"
  printf '{"total_count": %s, "policies": [%s]}\n' "$#" "${ids%, }" >"$STUB_API/repo_actions_policies"
}
repo=taojin-6/volumetric_kit_recon
add_policy="gh api -X POST repos/$repo/actions/policies --input - <<<'{\"name\": \"Only the owner can trigger workflows\", \"enforcement\": \"active\", \"conditions\": {\"workflow_path\": {\"include\": [\"~ALL\"], \"exclude\": []}}, \"rules\": [{\"type\": \"restrict_actions_actors\", \"parameters\": {\"allowed_actors\": [{\"id\": 22780239, \"type\": \"User\"}]}}]}'"
no_policy="No active Actions policy over every workflow lets only taojin-6 trigger them"
other_actor='{"id": 1, "type": "User"}'

api public
gate "a public repository whose one policy lets only its owner trigger every workflow, approving every outside contributor's run, gets runners" ok
expect "setup reads the repository, its policies, each by ID, and the approval of fork pull requests' runs" \
  "$(calls "^gh api repos/$repo --jq ") $(calls "^gh api --paginate repos/$repo/actions/policies\?per_page=100 --jq ") $(calls "^gh api repos/$repo/actions/policies/7054 --jq ") $(calls "^gh api repos/$repo/actions/permissions/fork-pr-contributor-approval --jq ")" \
  "1 1 1 1"
policies
gate "a public repository with no policy is refused, with the command that adds the owner-only one" refused \
  "is public, and a fork's pull request runs its own" "so no runner is registered until only the owner can send a job to" \
  "$no_policy" "policies: none" "$add_policy"
# The policy that command creates, as GitHub returns it unchanged, passes.
sed -n "s/^ *gh api -X POST .*<<<'\\(.*\\)'\$/\\1/p" "$tmp/out" | jq '{id: 7054} + .' >"$STUB_API/repo_actions_policies_7054"
policies 7054
gate "the policy the refusal's command creates passes the check as created" ok
policies 7054
policy 7054 active "$owner_actor, $other_actor" >"$STUB_API/repo_actions_policies_7054"
gate "a public repository whose policy lets another account trigger workflows too is refused, naming the policy and where to change it" refused \
  'policy 7054 "Policy 7054" lets actors other than the owner trigger workflows' \
  "https://github.com/$repo/settings/actions/rules/7054" "$no_policy" "$add_policy"
policy 7054 active '{"id": 5, "type": "RepositoryRole"}' >"$STUB_API/repo_actions_policies_7054"
gate "so is one whose policy lets a role trigger them" refused \
  'policy 7054 "Policy 7054" lets actors other than the owner trigger workflows'
policies 7054 9001
policy 7054 active "$owner_actor" >"$STUB_API/repo_actions_policies_7054"
policy 9001 active "$other_actor" >"$STUB_API/repo_actions_policies_9001"
gate "so is one with an owner-only policy and another, active, letting another account" refused \
  'policy 9001 "Policy 9001" lets actors other than the owner trigger workflows'
policy 9001 disabled "$other_actor" >"$STUB_API/repo_actions_policies_9001"
gate "a policy letting other accounts that is not active is no obstacle" ok
policy 9001 active "" >"$STUB_API/repo_actions_policies_9001"
policy 7054 active "" >"$STUB_API/repo_actions_policies_7054"
gate "a policy whose rule names no actor does not count" refused \
  "$no_policy" 'policy 7054 "Policy 7054" names no actor'
policies 7054
policy 7054 evaluate "$owner_actor" >"$STUB_API/repo_actions_policies_7054"
gate "an owner-only policy that is only evaluated, not enforced, does not count" refused \
  "$no_policy" 'policy 7054 "Policy 7054" is not active (enforcement: evaluate)'
for conditions in \
  '{"workflow_path": {"include": ["~ALL"], "exclude": [".github/workflows/ci.yml"]}}' \
  '{"workflow_path": {"include": [], "exclude": [".github/workflows/ci.yml"]}}' \
  '{"workflow_path": {"include": [".github/workflows/ci.yml"], "exclude": []}}' \
  '{"workflow_path": {"exclude": []}}' \
  '{"workflow_path": {"include": ["~ALL"], "exclude": []}, "ref_name": {"include": ["refs/heads/main"], "exclude": []}}' \
  '{}'; do
  policy 7054 active "$owner_actor" "$conditions" >"$STUB_API/repo_actions_policies_7054"
  gate "an owner-only policy over some workflows only, or whose conditions do not say which, does not count: $conditions" refused \
    "$no_policy" 'policy 7054 "Policy 7054" does not cover every workflow'
done
# GitHub: a condition that includes nothing includes every workflow no
# exclusion matches.
for conditions in \
  '{"workflow_path": {"include": [], "exclude": []}}' \
  '{"workflow_path": {"include": ["~ALL"]}}' \
  '{"workflow_path": {"include": ["~ALL"], "exclude": []}, "ref_name": {"include": [], "exclude": []}}'; do
  policy 7054 active "$owner_actor" "$conditions" >"$STUB_API/repo_actions_policies_7054"
  gate "an owner-only policy over every workflow counts: $conditions" ok
done
api public
echo '{"private": false, "owner": {"id": 7, "login": "someone-else"}}' >"$STUB_API/repo"
gate "a policy counts only if its actor is the repository's owner" refused \
  "No active Actions policy over every workflow lets only someone-else trigger them" \
  'policy 7054 "Policy 7054" lets actors other than the owner trigger workflows' \
  '"allowed_actors": [{"id": 7, "type": "User"}]'
for approval in first_time_contributors first_time_contributors_new_to_github; do
  api public
  echo "{\"approval_policy\": \"$approval\"}" >"$STUB_API/repo_actions_permissions_fork-pr-contributor-approval"
  gate "a public repository that approves fewer runs than every outside contributor's is refused, with the command that fixes it ($approval)" refused \
    "wait for approval only from $approval" \
    "gh api -X PUT repos/$repo/actions/permissions/fork-pr-contributor-approval -f approval_policy=all_external_contributors"
done
policies
gate "every gate missing is named in one refusal" refused "$no_policy" "wait for approval only from first_time_contributors_new_to_github"
api public
rm "$STUB_API/repo_actions_policies"
gate "a policy list gh cannot read refuses" refused "gh could not read repos/$repo/actions/policies?per_page=100"
api public
echo '{"owner": {"id": 22780239, "login": "taojin-6"}}' >"$STUB_API/repo"
gate "a repository that GitHub says is neither public nor private refuses" refused \
  "unexpected answer from GitHub about $repo: 'null 22780239 taojin-6'"
api public
STUB_GH_UNAUTHED=1 gate "without gh authed, registering is refused, asking for no token" refused \
  "registering needs gh, authed (gh auth login, or GH_TOKEN set)"

# A collaborator's push to a private repository runs its own workflow files,
# so it needs the same policies. It has no approval setting (GitHub answers
# 422), which setup does not read.
api private
gate "a private repository whose one policy lets only its owner trigger every workflow, with fork pull requests' workflows off, gets runners" ok
expect "setup reads the repository, its policies, each by ID, and whether fork pull requests' workflows run, not their approval" \
  "$(calls "^gh api repos/$repo --jq ") $(calls "^gh api --paginate repos/$repo/actions/policies\?per_page=100 --jq ") $(calls "^gh api repos/$repo/actions/policies/7054 --jq ") $(calls "^gh api repos/$repo/actions/permissions/fork-pr-workflows-private-repos --jq ") $(calls "fork-pr-contributor-approval")" \
  "1 1 1 1 0"
policies
gate "a private repository with no policy is refused, with the command that adds the owner-only one" refused \
  "is private, but a collaborator's push or fork's pull request runs its own" \
  "so no runner is registered until only the owner can send a job to" "$no_policy" "policies: none" "$add_policy"
policies 7054
policy 7054 active "$owner_actor, $other_actor" >"$STUB_API/repo_actions_policies_7054"
gate "a private repository whose policy lets another account trigger workflows too is refused" refused \
  'policy 7054 "Policy 7054" lets actors other than the owner trigger workflows' \
  "https://github.com/$repo/settings/actions/rules/7054" "$no_policy"
api private
policies 7054 9001
policy 9001 active "$other_actor" >"$STUB_API/repo_actions_policies_9001"
gate "so is one with an owner-only policy and another, active, letting another account" refused \
  'policy 9001 "Policy 9001" lets actors other than the owner trigger workflows'
api private
echo '{"run_workflows_from_fork_pull_requests": true}' \
  >"$STUB_API/repo_actions_permissions_fork-pr-workflows-private-repos"
gate "a private repository that runs fork pull requests' workflows is refused, with the command that turns them off" refused \
  "is private, but a collaborator's push or fork's pull request runs its own" \
  "It runs fork pull requests' workflows, which would reach these runners." \
  "gh api -X PUT repos/$repo/actions/permissions/fork-pr-workflows-private-repos -F run_workflows_from_fork_pull_requests=false"
policies
gate "every gate of a private repository missing is named in one refusal" refused \
  "$no_policy" "It runs fork pull requests' workflows"
rm "$STUB_API/repo_actions_permissions_fork-pr-workflows-private-repos"
gate "a private repository whose fork setting cannot be read is still refused without the policy" refused "$no_policy"
api private
rm "$STUB_API/repo_actions_permissions_fork-pr-workflows-private-repos"
gate "a private repository whose fork setting cannot be read gets runners, saying so" ok
expect "it says it could not read the setting" \
  "$(grep -c "^Could not read whether $repo runs fork pull requests' workflows; it is private, so going on\.\$" "$tmp/out")" 1
api private
rm "$STUB_API/repo_actions_policies"
gate "a private repository's policy list gh cannot read refuses" refused "gh could not read repos/$repo/actions/policies?per_page=100"
api public

# --- A Mac set up before: LaunchAgents ---------------------------------------

# A runner installed as a per-user LaunchAgent, whose .service names that
# agent, becomes a daemon, and the agent is unloaded and deleted, so no
# console login starts a second listener on the runner's credentials. So is
# the agent of a runner that is a daemon already, and an agent whose plist is
# gone but which is still loaded.
os=Darwin
export STUB_OS=Darwin
fresh_home
d=ci-runners/volumetric_kit_gfx/runner-1
runner "$d" gfx
runner ci-runners/volumetric_kit_gfx/runner-2 gfx "$(levels "$(share 3)")\n"
runner ci-runners/volumetric_kit_gfx/runner-3 gfx "$(levels "$(share 3)")\n"
unservice "$d"
agent "$d"
agent_plist "$d" >"$HOME/$d/.service"
agent ci-runners/volumetric_kit_gfx/runner-2
agent ci-runners/volumetric_kit_gfx/runner-3 none
status=0
setup gfx || status=$?
expect "a Mac's LaunchAgent runner becomes a daemon that runs" \
  "$status $(installed "$d") $(running "$d") $(cat "$HOME/$d/.service")" \
  "0 yes yes $(daemon_of "$d")"
expect "its agent is unloaded from the login session, waited for, and deleted" \
  "$(calls "^launchctl bootout gui/$(id -u)/$(label_of "$d")\$") $(agent_state "$d")" \
  "1 unloaded deleted"
expect "so is a daemon's leftover agent, and a loaded agent whose plist is gone" \
  "$(agent_state ci-runners/volumetric_kit_gfx/runner-2) $(agent_state ci-runners/volumetric_kit_gfx/runner-3)" \
  "unloaded deleted unloaded deleted"
expect "the runners that were daemons already are left running" \
  "$(calls '^launchctl (bootstrap|bootout|kickstart|enable) system/.*gfx\.runner-[23]') $(running ci-runners/volumetric_kit_gfx/runner-2)" \
  "0 yes"
: >"$CALLS"
status=0
setup gfx || status=$?
expect "a re-run with no agent left asks launchd to change nothing" \
  "$status $(calls '^launchctl (bootstrap|bootout|kickstart|enable) ')" "0 0"

# A daemon that cannot be written leaves the runner's agent loaded and in
# place, and no daemon at all, so the runner stays up and a re-run installs.
fresh_home
runner "$d" gfx
runner ci-runners/volumetric_kit_gfx/runner-2 gfx "$(levels "$(share 2)")\n"
unservice "$d"
agent "$d"
status=0
STUB_INSTALL_FAILS=1 setup gfx || status=$?
expect "a failed install fails the run, keeps the agent and leaves no daemon" \
  "$status $(grep -c '^error: 1 runner services failed' "$tmp/out") $(agent_state "$d") $(installed "$d") $([ -f "$HOME/$d/.service" ] && echo written || echo none)" \
  "1 1 loaded kept no none"

# --- calib and player, beside the other repositories -------------------------

# A 24-core Linux host and a 10-core Mac, faked on any host, each carrying
# every other repository's runners at a level of 2: calib's or player's
# register with the cores split across their own, 4 of 24 or 5 of 10, and the
# others' are left alone. calib goes last, so the cases after the loop start
# from the Mac with its runners at 5 beside the other three repositories'.
for os in Linux Darwin; do
  export STUB_OS="$os"
  if [ "$os" = Linux ]; then n=6 label=vk-linux-gpu ncores=24; else n=2 label=mac ncores=10; fi
  for repo in player calib; do
    fresh_home
    others=""
    for other in recon gfx calib player; do
      [ "$other" != "$repo" ] || continue
      others="$others${others:+|}$other"
      for i in $(seq 1 "$n"); do runner "ci-runners/volumetric_kit_$other/runner-$i" "$other" "$(levels 2)\n"; done
    done
    cache_tarball
    status=0
    STUB_CORES="$ncores" setup "$repo" || status=$?
    expect "$repo's runners register ($os)" "$status" 0
    [ "$status" -eq 0 ] || sed 's/^/     | /' "$tmp/out"
    registered=0 services=0 up=0
    for i in $(seq 1 "$n"); do
      d="ci-runners/volumetric_kit_$repo/runner-$i"
      grep -qF "config $d --unattended --url https://github.com/taojin-6/volumetric_kit_$repo --token REGTOKEN --labels $label --name $host-$repo-$i " "$CALLS" &&
        registered=$((registered + 1))
      [ "$(installed "$d")" = no ] || services=$((services + 1))
      [ "$(running "$d")" = no ] || up=$((up + 1))
    done
    expect "it registers $n, named and labelled $label, and installs and runs their services ($repo, $os)" \
      "$registered $(calls '^config ') $services $up" "$n $n $n $n"
    expect "each gets the cores split across $repo's $n runners: $((ncores / n)) of $ncores ($os)" \
      "$(env_of "ci-runners/volumetric_kit_$repo/runner-1") $(env_of "ci-runners/volumetric_kit_$repo/runner-$n")" \
      "$(levels $((ncores / n))) $(levels $((ncores / n)))"
    unchanged=0
    for d in "$HOME"/ci-runners/volumetric_kit_*/runner-*; do
      case "$d" in */volumetric_kit_"$repo"/*) continue ;; esac
      [ "$(env_of "${d#"$HOME"/}")" != "$(levels 2)" ] || unchanged=$((unchanged + 1))
    done
    expect "the other repositories' runners ($others) keep their levels and are not restarted ($repo, $os)" \
      "$unchanged $(touched "$others")" "$((3 * n)) 0"
  done
done
: >"$CALLS"
status=0
STUB_CORES=10 setup recon || status=$?
expect "a re-run for recon gives its runners 5 and restarts them, leaving the others' alone" \
  "$status $(env_of ci-runners/volumetric_kit_recon/runner-2) $(restarts ci-runners/volumetric_kit_recon/runner-1) $(restarts ci-runners/volumetric_kit_recon/runner-2) $(env_of ci-runners/volumetric_kit_gfx/runner-1) $(touched 'gfx|calib|player')" \
  "0 $(levels 5) 1 1 $(levels 2) 0"
status=0
STUB_CORES=1 setup calib || status=$?
expect "with fewer cores than runners, each gets 1" \
  "$status $(env_of ci-runners/volumetric_kit_calib/runner-1)" "0 $(levels 1)"
calib_daemon="$(daemon_of ci-runners/volumetric_kit_calib/runner-1)"
status=0
STUB_CORES=10 teardown calib || status=$?
remaining="" kept=""
for dir in "$HOME"/ci-runners/*/runner-*; do
  [ -d "$dir" ] || continue
  remaining="$remaining ${dir#"$HOME"/ci-runners/}"
  kept="$kept$(basename "$(daemon_of "${dir#"$HOME"/}")") "
done
expect "teardown removes calib's runners and their daemons, and only them" \
  "$status$remaining | $(daemons)" \
  "0 volumetric_kit_gfx/runner-1 volumetric_kit_gfx/runner-2 volumetric_kit_player/runner-1 volumetric_kit_player/runner-2 volumetric_kit_recon/runner-1 volumetric_kit_recon/runner-2 | $kept"
expect "it unloads each daemon before deleting it" \
  "$(calls "^launchctl bootout system/$(basename "$calib_daemon" .plist)\$") $([ -f "$calib_daemon" ] && echo kept || echo deleted)" \
  "1 deleted"

# --- MoltenVK_ROOT ----------------------------------------------------------

# The shell's MoltenVK_ROOT goes into every runner's .env, and a running runner
# restarts to read it; unset, each .env is kept, a MoltenVK_ROOT line too; and
# one naming no directory, or a relative one, stops setup, naming it, before
# anything changes.
os=Darwin
export STUB_OS=Darwin
fresh_home
p1=ci-runners/volumetric_kit_player/runner-1
p2=ci-runners/volumetric_kit_player/runner-2
runner "$p1" player "$(levels 5)\n"
runner "$p2" player "$(levels 5)\n"
mkdir -p "$tmp/MoltenVK"
status=0
MoltenVK_ROOT="$tmp/MoltenVK" STUB_CORES=10 setup player || status=$?
expect "MoltenVK_ROOT goes into every runner's .env" \
  "$status $(env_of "$p1" | tail -n 1) $(env_of "$p2" | tail -n 1)" \
  "0 MoltenVK_ROOT=$tmp/MoltenVK MoltenVK_ROOT=$tmp/MoltenVK"
expect "the running runners restart to read it" "$(restarts "$p1") $(restarts "$p2")" "1 1"
kept="$(env_of "$p1")"
: >"$CALLS"
status=0
STUB_CORES=10 setup player || status=$?
expect "with MoltenVK_ROOT unset, each .env is kept, and no runner restarts" \
  "$status $([ "$(env_of "$p1")" = "$kept" ] && echo kept) $(changes)" "0 kept 0"
: >"$CALLS"
status=0
MoltenVK_ROOT="$tmp/no-such-dir" STUB_CORES=10 setup player || status=$?
expect "a MoltenVK_ROOT naming no directory stops setup, naming it, before any change" \
  "$status $(grep -c "^error: MoltenVK_ROOT is not a directory: $tmp/no-such-dir\$" "$tmp/out") $([ "$(env_of "$p1")" = "$kept" ] && echo kept) $(calls '^(config|gh|curl|launchctl) ')" \
  "1 1 kept 0"
: >"$CALLS"
status=0
(cd "$tmp" && MoltenVK_ROOT=MoltenVK STUB_CORES=10 setup player) || status=$?
expect "a relative MoltenVK_ROOT stops setup, naming it, before any change" \
  "$status $(grep -c '^error: MoltenVK_ROOT is not an absolute path: MoltenVK$' "$tmp/out") $([ "$(env_of "$p1")" = "$kept" ] && echo kept) $(calls '^(config|gh|curl|launchctl) ')" \
  "1 1 kept 0"

# --- Any other repository, as owner/repo -------------------------------------

# A repository named in full gets runners as the family's do, in
# ~/ci-runners/<repo> and named after the repository, beside the family's
# runners and another owner's repository of the same name, whose runners share
# the directory; setup and teardown leave both alone.
for os in Linux Darwin; do
  export STUB_OS="$os"
  if [ "$os" = Linux ]; then n=6 label=vk-linux-gpu; else n=2 label=mac; fi
  fresh_home
  for i in $(seq 1 "$n"); do runner "ci-runners/volumetric_kit_recon/runner-$i" recon "$(levels 2)\n"; done
  other=ci-runners/example-repo/runner-1
  runner "$other" other-owner/example-repo "$(levels 2)\n"
  # The other owner's service calls, reading its state included.
  if [ "$os" = Linux ]; then other_calls="^svc $other "; else other_calls='^launchctl .*other-owner-example-repo\.'; fi
  cache_tarball
  status=0
  setup example-owner/example-repo || status=$?
  expect "a repository named in full registers ($os)" "$status" 0
  [ "$status" -eq 0 ] || sed 's/^/     | /' "$tmp/out"
  registered=0 services=0 up=0
  for i in $(seq 2 $((n + 1))); do # runner-1 is the other owner's
    d="ci-runners/example-repo/runner-$i"
    grep -qF "config $d --unattended --url https://github.com/example-owner/example-repo --token REGTOKEN --labels $label --name $host-example-repo-$i " "$CALLS" &&
      registered=$((registered + 1))
    [ "$(installed "$d")" = no ] || services=$((services + 1))
    [ "$(running "$d")" = no ] || up=$((up + 1))
  done
  expect "it registers $n in ~/ci-runners/example-repo, named after the repository and labelled $label, and installs and runs their services ($os)" \
    "$registered $(calls '^config ') $(calls '^gh api -X POST repos/example-owner/example-repo/actions/runners/registration-token ') $services $up" \
    "$n $n 1 $n $n"
  [ "$os" = Linux ] ||
    expect "their daemons carry the owner and repository in their labels (Darwin)" \
      "$(label_of ci-runners/example-repo/runner-2)" \
      "actions.runner.example-owner-example-repo.$host-example-repo-2"
  expect "each gets the cores split across the repository's own runners ($os)" \
    "$(env_of ci-runners/example-repo/runner-2) $(env_of "ci-runners/example-repo/runner-$((n + 1))")" \
    "$(levels "$(share "$n")") $(levels "$(share "$n")")"
  expect "the family's runners and the other owner's keep their levels, are not restarted, and run ($os)" \
    "$(env_of ci-runners/volumetric_kit_recon/runner-1) $(env_of "$other") $(touched recon) $(calls "$other_calls") $(running "$other")" \
    "$(levels 2) $(levels 2) 0 0 yes"
  : >"$CALLS"
  status=0
  setup example-owner/example-repo || status=$?
  expect "a re-run asks gh for nothing, registers nothing and changes no service ($os)" \
    "$status $(calls '^(config|gh|curl) ') $(changes)" "0 0 0"
  # GitHub's names ignore case, so another spelling is the same repository:
  # setup finds its runners rather than registering a second set, and teardown
  # (below) removes them.
  : >"$CALLS"
  status=0
  setup Example-Owner/Example-Repo || status=$?
  expect "a re-run spelling the name in other case finds the same runners ($os)" \
    "$status $(calls '^(config|gh|curl) ') $(changes)" "0 0 0"

  : >"$CALLS"
  status=0
  teardown EXAMPLE-OWNER/example-repo || status=$?
  remaining=""
  for dir in "$HOME"/ci-runners/*/runner-*; do
    [ -d "$dir" ] && remaining="$remaining ${dir#"$HOME"/ci-runners/}"
  done
  expect "teardown deregisters and deletes the repository's runners, and only them ($os)" \
    "$status $(calls '^config ci-runners/example-repo/.* remove --token RMTOKEN$') $(calls '^gh api -X POST repos/example-owner/example-repo/actions/runners/remove-token ')$remaining" \
    "0 $n 1 example-repo/runner-1$(for i in $(seq 1 "$n"); do printf ' volumetric_kit_recon/runner-%s' "$i"; done)"
  expect "it leaves the other owner's runner and the family's running ($os)" \
    "$(calls "$other_calls") $(touched recon) $(running "$other") $(running ci-runners/volumetric_kit_recon/runner-1)" \
    "0 0 yes yes"
done

# --- Another account, with --user ------------------------------------------

# With --user, the runners are another account's: registered, their files
# written and deregistered as it, through sudo, in its home, and their
# services run as it. This user's runners of other repositories are left
# alone; its own of the same repository stop setup, since the account's would
# take over their names. An account that does not exist, or a name that is no
# login name, is refused before anything is touched; this user's own name is
# no other account.
account=ci
# in_account CMD...: CMD with HOME the account's, for the helpers above.
in_account() {
  local HOME="$STUB_HOMES/$account"
  "$@"
}
for os in Linux Darwin; do
  export STUB_OS="$os"
  if [ "$os" = Linux ]; then n=6 label=vk-linux-gpu; else n=2 label=mac; fi
  fresh_home
  mkdir -p "$STUB_HOMES/$account"
  runner ci-runners/volumetric_kit_gfx/runner-1 gfx "$(levels 2)\n"
  in_account cache_tarball
  mkdir -p "$tmp/MoltenVK"
  status=0
  MoltenVK_ROOT="$tmp/MoltenVK" setup --user "$account" recon || status=$?
  expect "setup for another account succeeds ($os)" "$status" 0
  [ "$status" -eq 0 ] || sed 's/^/     | /' "$tmp/out"
  registered=0 services=0 up=0
  for i in $(seq 1 "$n"); do
    d="ci-runners/volumetric_kit_recon/runner-$i"
    grep -qF "config $d --unattended --url https://github.com/taojin-6/volumetric_kit_recon --token REGTOKEN --labels $label --name $host-recon-$i " "$CALLS" &&
      registered=$((registered + 1))
    [ "$(in_account installed "$d")" = no ] || services=$((services + 1))
    [ "$(in_account running "$d")" = no ] || up=$((up + 1))
  done
  expect "it registers $n runners in the account's home, as the account, and installs and runs their services ($os)" \
    "$registered $(calls '^config ') $services $up $([ -d "$HOME/ci-runners/volumetric_kit_recon" ] && echo mine || echo none)" \
    "$n $n $n $n none"
  expect "they get the cores split across them, and MoltenVK_ROOT ($os)" \
    "$(in_account env_of ci-runners/volumetric_kit_recon/runner-1)" \
    "$(levels "$(share "$n")")
MoltenVK_ROOT=$tmp/MoltenVK"
  ahome="$STUB_HOMES/$account"
  if [ "$os" = Linux ]; then
    expect "each service is installed as the account (Linux)" \
      "$(calls "^svc $ahome/ci-runners/volumetric_kit_recon/runner-[0-9]* install $account\$")" "$n"
  else
    d=ci-runners/volumetric_kit_recon/runner-1
    daemon="$(in_account daemon_of "$d")"
    expect "each daemon runs the runner as the account, with its HOME and logs (Darwin)" \
      "$(grep -c -F -e "<string>$ahome/$d/runsvc.sh</string>" -e "<key>UserName</key><string>$account</string>" \
        -e "<key>HOME</key><string>$ahome</string>" -e "<key>USER</key><string>$account</string>" \
        -e "<string>$ahome/Library/Logs/$(in_account label_of "$d")/stdout.log</string>" "$daemon") $([ -d "$ahome/Library/Logs/$(in_account label_of "$d")" ] && echo logs)" \
      "5 logs"
  fi
  expect "this user's runners of another repository are left alone ($os)" \
    "$(touched gfx) $(env_of ci-runners/volumetric_kit_gfx/runner-1) $(running ci-runners/volumetric_kit_gfx/runner-1)" \
    "0 $(levels 2) yes"
  expect "it says whom they run as ($os)" "$(grep -c "^Done: $n runners .*, run as $account\.\$" "$tmp/out")" 1

  : >"$CALLS"
  status=0
  setup --user "$account" recon || status=$?
  expect "a re-run for the account asks gh for nothing, registers nothing and changes no service ($os)" \
    "$status $(calls '^(config|gh|curl) ') $(changes)" "0 0 0"

  runner ci-runners/volumetric_kit_recon/runner-1 recon
  : >"$CALLS"
  status=0
  setup --user "$account" recon || status=$?
  expect "this user's own runners of the repository stop setup for the account, before any change ($os)" \
    "$status $(grep -c "has runners of taojin-6/volumetric_kit_recon here: tear them down first" "$tmp/out") $(calls '^(config|svc|gh|curl|launchctl) ')" \
    "1 1 0"
  rm -f "$(daemon_of ci-runners/volumetric_kit_recon/runner-1)"
  rm -rf "$HOME/ci-runners/volumetric_kit_recon"

  : >"$CALLS"
  status=0
  teardown --user "$account" recon || status=$?
  remaining=""
  for dir in "$ahome"/ci-runners/*/runner-* "$HOME"/ci-runners/*/runner-*; do
    [ -d "$dir" ] && remaining="$remaining ${dir#"$tmp"/}"
  done
  expect "teardown for the account deregisters and deletes its runners, and only them ($os)" \
    "$status $(calls '^config ci-runners/volumetric_kit_recon/runner-[0-9]* remove --token RMTOKEN$')$remaining | $(daemons)" \
    "0 $n home/ci-runners/volumetric_kit_gfx/runner-1 | $(basename "$(daemon_of ci-runners/volumetric_kit_gfx/runner-1)") "
  expect "it leaves this user's runner running ($os)" \
    "$(touched gfx) $(running ci-runners/volumetric_kit_gfx/runner-1)" "0 yes"
done

# Refused: no such account, or no login name; nothing is touched. Each name
# but the first has a home, so only the name check refuses it. This user's own
# name sets up this user's runners, with no sudo to any account.
export STUB_OS=Linux os=Linux
fresh_home
mkdir -p "$STUB_HOMES/-x" "$STUB_HOMES/c i" "$STUB_HOMES/../ci"
for name in nobody-here -x 'c i' '../ci' ''; do
  status=0
  setup --user "$name" recon || status=$?
  expect "setup refuses the account '$name', touching nothing" \
    "$status $(calls '^(config|svc|gh|curl|launchctl) ')" "2 0"
  status=0
  teardown --user "$name" recon || status=$?
  expect "teardown refuses the account '$name', touching nothing" \
    "$status $(calls '^(config|svc|gh|curl|launchctl) ')" "2 0"
done
# A MoltenVK_ROOT the account cannot read stops setup before any change.
mkdir -p "$STUB_HOMES/$account" "$tmp/closed/MoltenVK"
: >"$CALLS"
status=0
MoltenVK_ROOT="$tmp/closed/MoltenVK" STUB_CLOSED="$tmp/closed" setup --user "$account" player || status=$?
expect "a MoltenVK_ROOT the account cannot read stops setup, naming it, before any change" \
  "$status $(grep -c "^error: MoltenVK_ROOT is not a directory: $tmp/closed/MoltenVK\$" "$tmp/out") $(calls '^(config|svc|gh|curl|launchctl) ')" \
  "1 1 0"
cache_tarball
: >"$CALLS"
status=0
setup --user "$user" recon || status=$?
expect "--user naming this user sets up this user's runners, as plain setup does" \
  "$status $(calls '^config ci-runners/volumetric_kit_recon/runner-') $(calls '^sudo -u ')" "0 6 0"

# --- Teardown ---------------------------------------------------------------

for os in Linux Darwin; do
  export STUB_OS="$os"
  fresh_home
  runner ci-runners/volumetric_kit_recon/runner-1 recon
  runner ci-runners/volumetric_kit_recon/runner-2 ""
  runner actions-runner-recon-3 recon
  runner actions-runner-recon-4 "" # its service left installed
  if [ "$os" = Linux ]; then
    cp "$runner_files/svc.sh" "$HOME/actions-runner-recon-4/"
    echo unit >"$HOME/actions-runner-recon-4/.service"
  else
    # Its daemon still loaded and named only by .service; and runner-1 still
    # has an earlier setup's agent.
    leftover="$RUNNER_DAEMON_DIR/actions.runner.taojin-6-volumetric_kit_recon.leftover.plist"
    printf '<key>WorkingDirectory</key><string>%s</string>\n' "$HOME/actions-runner-recon-4" >"$leftover"
    echo "$leftover" >"$HOME/actions-runner-recon-4/.service"
    touch "$HOME/actions-runner-recon-4/.loaded" "$HOME/actions-runner-recon-4/.running"
    agent ci-runners/volumetric_kit_recon/runner-1
    old_agent="$(agent_plist ci-runners/volumetric_kit_recon/runner-1) $(agent_mark ci-runners/volumetric_kit_recon/runner-1)"
  fi
  runner actions-runner-1 gfx
  runner ci-runners/volumetric_kit_gfx/runner-1 gfx
  status=0
  teardown recon || status=$?
  expect "teardown succeeds ($os)" "$status" 0
  [ "$status" -eq 0 ] || sed 's/^/     | /' "$tmp/out"
  remaining=""
  for dir in "$HOME"/ci-runners/*/runner-* "$HOME"/actions-runner-*; do
    [ -d "$dir" ] && remaining="$remaining ${dir#"$HOME"/}"
  done
  expect "it deletes the repository's runners and leftovers, and only them ($os)" "$remaining" \
    " ci-runners/volumetric_kit_gfx/runner-1 actions-runner-1"
  expect "it deregisters each registered runner ($os)" \
    "$(calls '^config .* remove --token RMTOKEN$') $(calls '^config ci-runners/volumetric_kit_recon/runner-2')" "2 0"
  if [ "$os" = Linux ]; then
    expect "it uninstalls every service of theirs, a leftover's too (Linux)" "$(calls ' uninstall$')" 3
  else
    expect "it unloads and deletes each daemon of theirs, a leftover's too, and only theirs (Darwin)" \
      "$(calls '^launchctl bootout system/') $(daemons)" \
      "3 $(basename "$(daemon_of actions-runner-1)") $(basename "$(daemon_of ci-runners/volumetric_kit_gfx/runner-1)") "
    remaining=""
    # shellcheck disable=SC2086 # the plist and the stub's mark, split
    for f in $old_agent; do [ ! -f "$f" ] || remaining="$remaining $f"; done
    expect "it unloads and deletes a runner's agent (Darwin)" "$remaining" ""
  fi
  expect "it checks gh once and mints one remove token ($os)" \
    "$(calls '^gh auth status$') $(calls remove-token)" "1 1"

  # A leftover whose daemon will not unload keeps its directory, and the run
  # fails: deleted, the daemon would be restarted, failing, for good.
  if [ "$os" = Darwin ]; then
    fresh_home
    d=ci-runners/volumetric_kit_recon/runner-1
    runner "$d" recon
    rm "$HOME/$d/.runner"
    status=0
    STUB_BOOTOUT_FAILS=1 teardown recon || status=$?
    expect "a leftover whose daemon does not unload is kept, and teardown fails (Darwin)" \
      "$status $([ -d "$HOME/$d" ] && echo kept) $(installed "$d")" "1 kept yes"
  fi

  for case in STUB_REMOVE_FAILS STUB_GH_UNAUTHED; do
    fresh_home
    runner ci-runners/volumetric_kit_recon/runner-1 recon
    status=0
    env "$case=1" "$BASH" "$tools/teardown-runners.sh" recon >"$tmp/out" 2>&1 || status=$?
    expect "teardown fails when it cannot deregister ($case, $os)" "$status" 1
    if [ -f "$HOME/ci-runners/volumetric_kit_recon/runner-1/.runner" ]; then
      pass "it keeps the runner's credentials for a retry ($case, $os)"
    else
      fail "it keeps the runner's credentials for a retry ($case, $os)"
    fi
  done
  expect "without gh auth it leaves the runner running ($os)" \
    "$(changes) $(running ci-runners/volumetric_kit_recon/runner-1)" "0 yes"

  fresh_home
  runner actions-runner-1 gfx
  status=0
  teardown recon || status=$?
  expect "teardown with nothing to remove succeeds and touches nothing ($os)" \
    "$status $(calls '^(svc|config|gh|launchctl) ')" "0 0"
done
unset STUB_OS

export HOME="$real_home"
report
