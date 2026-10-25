# shellcheck shell=bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
#
# Sourced by setup-runners.sh and teardown-runners.sh: which repository a
# runner belongs to, which account it runs as, where its files live, and how
# its service is driven. README.md describes the layout.

OS="$(uname -s)"
: "${USER:=$(id -un)}" # not every shell sets it

die() {
  echo "error: $*" >&2
  exit 1
}

# The account the runners run as and its home: this user, or with --user
# another account, whose files this user reaches through sudo (README.md, "A
# dedicated account"). Every repository's runners live under one directory
# there, each repository's in its own subdirectory, so several repositories
# share a host without colliding.
RUNNER_USER="$USER"
RUNNER_HOME="$HOME"
BASE="$RUNNER_HOME/ci-runners"

# select_account NAME: the runners run as account NAME, or exits 2 if there
# is no such account. Before select_repo, which places them in its home.
select_account() {
  local home
  [ "$1" != "$USER" ] || return 0
  # A login name, so it can be neither a sudo option nor a path.
  if ! [[ $1 =~ ^[a-z_][a-z0-9_-]*$ ]] ||
    ! home="$(sudo -u "$1" -H sh -c 'printf %s "$HOME"')" || [ -z "$home" ]; then
    echo "no account '$1' to run the runners as" >&2
    exit 2
  fi
  RUNNER_USER="$1"
  RUNNER_HOME="$home"
  BASE="$RUNNER_HOME/ci-runners"
}

# as_runner CMD...: CMD as the runners' account, on this shell's PATH, which
# registering records as the runners' own (README.md, "Macs").
as_runner() {
  if [ "$RUNNER_USER" = "$USER" ]; then
    "$@"
  else
    sudo -u "$RUNNER_USER" -H env PATH="$PATH" "$@"
  fi
}

# in_dir DIR CMD...: CMD run in DIR as the runners' account.
in_dir() {
  # shellcheck disable=SC2016 # expanded by the inner shell
  as_runner sh -c 'cd "$1" && shift && exec "$@"' in_dir "$@"
}

# dirs_of PREFIX...: each directory whose path is PREFIX and more, as the
# runners' account sees them, one a line, without the trailing slash.
dirs_of() {
  # shellcheck disable=SC2016 # expanded by the inner shell
  as_runner sh -c 'for p; do for d in "$p"*/; do
    if [ -d "$d" ]; then printf "%s\n" "${d%/}"; fi
  done; done' dirs_of "$@"
}

# Sets REPO, SLUG and RUNNER_ROOT for a repository, or exits 2: one of the
# family's by its short name, or any other in full, as owner/repo, in lower
# case. SLUG, which names the runners, is the short name or the repository's
# name.
select_repo() {
  # GitHub's characters: an owner's letters, digits, hyphens and underscores,
  # a repository's dots too. A name of dots alone would put the runners
  # outside ~/ci-runners.
  local full='^[A-Za-z0-9_-]+/[A-Za-z0-9._-]+$'
  case "${1:-}" in
    recon | gfx | calib | player)
      REPO="taojin-6/volumetric_kit_$1"
      ;;
    *)
      if ! [[ ${1:-} =~ $full ]] || [ "${1#*/}" = . ] || [ "${1#*/}" = .. ]; then
        echo "unknown repository '${1:-}': expected recon, gfx, calib, player or owner/repo" >&2
        exit 2
      fi
      # GitHub ignores case in a name, so every spelling of a repository must
      # find the same runners: one directory, and the URL their .runner keeps.
      REPO="$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')"
      ;;
  esac
  # shellcheck disable=SC2034 # setup-runners.sh names runners with it
  SLUG="$(printf '%s' "${1#*/}" | tr '[:upper:]' '[:lower:]')"
  RUNNER_ROOT="$BASE/${REPO#*/}"
}

# select_args ARGS...: the scripts' arguments, [--user <account>] <repo>: the
# account, then the repository, or exits 2.
select_args() {
  if [ "${1:-}" = --user ] && [ $# -ge 2 ]; then
    select_account "$2"
    shift 2
  fi
  [ $# -eq 1 ] || {
    echo "usage: $0 [--user <account>] <recon|gfx|calib|player|owner/repo>" >&2
    exit 2
  }
  select_repo "$1"
}

# Each runner is a service of the host's init system, run as the runners'
# account: systemd's, through the runner's own svc.sh, on Linux, and on macOS
# a system LaunchDaemon in DAEMON_DIR, which comes up at boot with nobody
# logged in (DECISIONS.md, 2026-10-17). The tests point DAEMON_DIR elsewhere.
DAEMON_DIR="${RUNNER_DAEMON_DIR:-/Library/LaunchDaemons}"

# svc DIR ARGS...: the runner's own service script, run from its directory as
# root (Linux only). Root changes directory itself: another account's home
# may be closed to this user.
svc() {
  # shellcheck disable=SC2016 # expanded by the inner shell
  sudo sh -c 'cd "$1" && shift && exec ./svc.sh "$@"' svc "$@"
}

# daemon_label DIR: runner $1's service name, which registering wrote into its
# svc.sh: actions.runner.<owner>-<repo>.<runner name>. Without svc.sh, the
# name of the plist its .service records, a daemon's or a LaunchAgent's; empty
# for a runner that never finished registering.
daemon_label() {
  local label service
  label="$(as_runner sed -n 's/^SVC_NAME="\(.*\)"$/\1/p' "$1/svc.sh" 2>/dev/null | head -n 1 | tr ' ' _)"
  if [ -z "$label" ] && as_runner test -f "$1/.service"; then
    service="$(as_runner head -n 1 "$1/.service")"
    case "$service" in */*.plist) label="$(basename "$service" .plist)" ;; esac
  fi
  echo "$label"
}

# daemon_xml DIR LABEL: the LaunchDaemon that runs runner $1: the runner's own
# LaunchAgent template as the runners' account, plus KeepAlive, which restarts
# a listener that exits, and the HOME and USER a login session would give it.
daemon_xml() {
  local logs="$RUNNER_HOME/Library/Logs/$2"
  cat <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$2</string>
  <key>ProgramArguments</key><array><string>$1/runsvc.sh</string></array>
  <key>UserName</key><string>$RUNNER_USER</string>
  <key>WorkingDirectory</key><string>$1</string>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>$logs/stdout.log</string>
  <key>StandardErrorPath</key><string>$logs/stderr.log</string>
  <key>EnvironmentVariables</key>
  <dict>
    <key>ACTIONS_RUNNER_SVC</key><string>1</string>
    <key>HOME</key><string>$RUNNER_HOME</string>
    <key>USER</key><string>$RUNNER_USER</string>
  </dict>
  <key>ProcessType</key><string>Interactive</string>
  <key>SessionCreate</key><true/>
</dict>
</plist>
EOF
}

# Whether launchd has runner $1's daemon loaded, running or not.
daemon_loaded() {
  local label
  label="$(daemon_label "$1")"
  [ -n "$label" ] && target_loaded "system/$label"
}

daemon_gone() { ! daemon_loaded "$1"; }

# wait_for SECONDS CMD...: whether CMD succeeds within about SECONDS.
wait_for() {
  local tries="$1"
  shift
  until "$@"; do
    tries=$((tries - 1))
    [ "$tries" -gt 0 ] || return 1
    sleep 1
  done
}

# Whether launchd service target $1 is loaded.
target_loaded() { launchctl print "$1" >/dev/null 2>&1; }
target_gone() { ! target_loaded "$1"; }

# remove_launch_agent DIR: unloads and deletes the per-user LaunchAgent an
# earlier setup installed for runner $1, which would start a second listener
# on its credentials at the next console login. The plist goes only once the
# agent is unloaded, so a failed attempt is retried. Does nothing on Linux.
remove_launch_agent() {
  local label agent target
  [ "$OS" = Darwin ] || return 0
  label="$(daemon_label "$1")"
  [ -n "$label" ] || return 0
  agent="$RUNNER_HOME/Library/LaunchAgents/$label.plist"
  target="gui/$(as_runner id -u)/$label"
  as_runner test -f "$agent" || target_loaded "$target" || return 0
  # As root, which reaches a login session from SSH too; with none, nothing runs.
  sudo launchctl bootout "$target" 2>/dev/null || true
  wait_for 30 target_gone "$target" || {
    echo "error: launchd still has $target loaded" >&2
    return 1
  }
  as_runner rm -f "$agent"
}

# Whether runner $1's service is installed: on macOS whether its daemon is,
# whatever .service says, which on a host set up before names its LaunchAgent.
service_installed() {
  local label
  if [ "$OS" = Linux ]; then
    as_runner test -f "$1/.service"
  else
    label="$(daemon_label "$1")"
    [ -n "$label" ] && [ -f "$DAEMON_DIR/$label.plist" ]
  fi
}

# install_service DIR: installs runner $1's service, stopped. On macOS the
# daemon is written whole or not at all, since an installed one is never
# rewritten, and .service is written as svc.sh does: the runner will not
# deregister while it exists.
install_service() {
  local label plist tmp
  if [ "$OS" = Linux ]; then
    svc "$1" install "$RUNNER_USER" # as the runners' account, not root
    return
  fi
  label="$(daemon_label "$1")"
  [ -n "$label" ] || {
    echo "error: $1/svc.sh names no service" >&2
    return 1
  }
  plist="$DAEMON_DIR/$label.plist"
  tmp="$(mktemp)" || return 1
  if as_runner mkdir -p "$RUNNER_HOME/Library/Logs/$label" &&
    as_runner cp "$1/bin/runsvc.sh" "$1/runsvc.sh" && as_runner chmod u+x "$1/runsvc.sh" &&
    daemon_xml "$1" "$label" >"$tmp" && plutil -lint "$tmp" >/dev/null &&
    sudo cp "$tmp" "$plist" && sudo chmod 644 "$plist" &&
    echo "$plist" | as_runner tee "$1/.service" >/dev/null; then
    rm -f "$tmp"
    return 0
  fi
  rm -f "$tmp"
  sudo rm -f "$plist"
  return 1
}

# uninstall_service DIR: stops runner $1's service and uninstalls it, and on
# macOS its LaunchAgent too. Does nothing to a runner with none.
uninstall_service() {
  local label
  if [ "$OS" = Linux ]; then
    ! as_runner test -f "$1/.service" || svc "$1" uninstall
    return
  fi
  label="$(daemon_label "$1")"
  if [ -n "$label" ]; then
    stop_service "$1" && sudo rm -f "$DAEMON_DIR/$label.plist" && remove_launch_agent "$1" || return 1
  fi
  as_runner rm -f "$1/.service"
}

# Whether runner directory $1 is registered to $REPO. Its .runner records the
# repository URL; the closing quote stops one name matching another's prefix.
registered_to_repo() {
  as_runner grep -qsF "github.com/${REPO}\"" "${1%/}/.runner"
}

# Fills RUNNER_DIRS with this repository's registered runners on this host:
# each ~/ci-runners/<repo>/runner-*, and each flat ~/actions-runner-* of hosts
# set up before that layout, whose .runner names $REPO -- that glob matches
# every repository's. ~ is the runners' account's home.
find_runner_dirs() {
  RUNNER_DIRS=()
  local dir
  while IFS= read -r dir; do
    if registered_to_repo "$dir"; then RUNNER_DIRS+=("$dir"); fi
  done < <(dirs_of "$RUNNER_ROOT/runner-" "$RUNNER_HOME/actions-runner-")
}

# The awk program of set_env: KEY=VALUE in place of the first KEY line, other
# KEY lines dropped, every other line kept.
# shellcheck disable=SC2016 # awk's own variables
SET_ENV_AWK='
  index($0, k "=") == 1 { if (!done) print k "=" v; done = 1; next }
  { print }
  END { if (!done) print k "=" v }
'

# set_env FILE KEY VALUE: makes KEY=VALUE the only KEY line in FILE, in place
# of the first old one, keeping every other line, as the runners' account.
# Sets ENV_CHANGED=1 when the file changes.
set_env() {
  local out
  # shellcheck disable=SC2016 # expanded by the inner shell
  out="$(as_runner sh -c '
    touch "$1" && tmp="$(mktemp "$1.XXXXXX")" || exit 1
    awk -v k="$2" -v v="$3" "$4" "$1" >"$tmp" || { rm -f "$tmp"; exit 1; }
    if cmp -s "$tmp" "$1"; then rm -f "$tmp"; else mv -f "$tmp" "$1" && echo changed; fi
  ' set_env "$1" "$2" "$3" "$SET_ENV_AWK")" || return 1
  # shellcheck disable=SC2034 # setup-runners.sh reads it
  [ "$out" != changed ] || ENV_CHANGED=1
}

# Whether runner directory $1's service is running: on Linux what svc.sh
# prints of systemd's state, as it exits 0 either way, and on macOS launchd's.
service_running() {
  local out label
  service_installed "$1" || return 1
  if [ "$OS" = Linux ]; then
    out="$(svc "$1" status 2>/dev/null)" || return 1
    case "$out" in *"Active: active "*) return 0 ;; esac
    return 1
  fi
  label="$(daemon_label "$1")"
  out="$(launchctl print "system/$label" 2>/dev/null)" || return 1
  grep -q '^[[:space:]]*state = running' <<<"$out"
}

# Starts runner directory $1's service and checks that it runs. A daemon
# launchd already has loaded is kicked rather than loaded again, which it
# refuses.
start_service() {
  local label
  if [ "$OS" = Linux ]; then
    svc "$1" start >/dev/null
    service_running "$1" && return 0
    echo "error: the runner in $1 did not start; './svc.sh status' there says why" >&2
    return 1
  fi
  label="$(daemon_label "$1")"
  sudo launchctl enable "system/$label"
  if daemon_loaded "$1"; then
    sudo launchctl kickstart "system/$label"
  else
    sudo launchctl bootstrap system "$DAEMON_DIR/$label.plist"
  fi
  wait_for 10 service_running "$1" && return 0
  echo "error: the runner in $1 did not start; 'launchctl print system/$label' says why" >&2
  return 1
}

# Stops runner directory $1's service. On macOS a stopped daemon is unloaded,
# so it stays down until the host restarts, and this waits until launchd lets
# go of it: loading it again any sooner fails ("Bootstrap failed: 5").
stop_service() {
  local label
  if [ "$OS" = Linux ]; then
    svc "$1" stop >/dev/null
    return
  fi
  label="$(daemon_label "$1")"
  daemon_loaded "$1" || return 0
  sudo launchctl bootout "system/$label" 2>/dev/null || true
  wait_for 30 daemon_gone "$1" && return 0
  echo "error: launchd still has the runner in $1 loaded ('launchctl print system/$label')" >&2
  return 1
}

# Restarts runner directory $1's service, so it reads its .env again; this
# cancels a job it is running. On macOS in place, with no unload.
restart_service() {
  if [ "$OS" = Linux ]; then
    stop_service "$1" && start_service "$1"
    return
  fi
  sudo launchctl kickstart -k "system/$(daemon_label "$1")" &&
    wait_for 10 service_running "$1"
}
