#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
#
# Runs homebrew-key.sh against a stub brew that lists a fake set of installed
# formulae, and checks when its key changes and when it does not. Where brew
# itself is installed on macOS, it also runs the script against that, which
# only reads, for an installed formula with dependencies, and checks they are
# hashed. The script runs under the bash running this, so
# `/bin/bash test_homebrew_key.sh` tests it under macOS's bash 3.2.
# shellcheck disable=SC2016 # the stubs' variables expand when the stubs run
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
script="$here/../homebrew-key.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
# shellcheck source-path=SCRIPTDIR source=lib.sh
. "$here/lib.sh"
real_path="$PATH"

# --- Stubs ------------------------------------------------------------------

# brew answers from $BREW_STATE: `versions` has a line "name version..." per
# installed formula, in the order brew prints its versions; `deps` a line
# "name dependency..." per formula, its runtime dependencies, recursively, as
# its keg records them; `outdated` a line per formula installed at a version
# older than Homebrew's metadata; `aliases` a line "alias name" per alias.
# Like real brew, `deps --installed` leaves out a dependency that is not
# installed or is outdated, and `list` aborts, listing nothing, on a
# tap-qualified name (no tap is tapped). It logs every call, and the
# Homebrew variables it saw to $ENVS; prints $STUB_PREFIX (default
# /opt/homebrew) for --prefix; fails `list`, `deps` or --prefix when
# STUB_LIST_FAILS, STUB_DEPS_FAILS or STUB_PREFIX_FAILS is set; and warns
# from a `list` that succeeds when STUB_WARNS is. `uname -s` names $STUB_OS
# (default Darwin).
stubs="$tmp/stubs"
mkdir -p "$stubs"
export CALLS="$tmp/calls.log" ENVS="$tmp/envs.log" BREW_STATE="$tmp/state"
printf '%s\n' '#!/bin/sh' 'echo "${STUB_OS:-Darwin}"' >"$stubs/uname"
cat >"$stubs/brew" <<'EOF'
#!/bin/sh
echo "brew $*" >>"$CALLS"
echo "${HOMEBREW_NO_AUTO_UPDATE:-unset} ${HOMEBREW_NO_ENV_HINTS:-unset}" >>"$ENVS"
canonical() {
  awk -v a="$1" '$1 == a { n = $2 } END { print (n == "" ? a : n) }' "$BREW_STATE/aliases"
}
installed() { awk -v n="$1" '$1 == n' "$BREW_STATE/versions"; }
outdated() { grep -qx "$1" "$BREW_STATE/outdated"; }
failure() { echo "Error: stub failure" >&2; exit 1; }
deps() {
  for f in "$@"; do
    awk -v n="$(canonical "$f")" '$1 == n { $1 = ""; print }' "$BREW_STATE/deps"
  done | tr ' ' '\n' | sed '/^$/d' | sort -u
}
case "$*" in
  --prefix)
    [ -z "${STUB_PREFIX_FAILS:-}" ] || failure
    echo "${STUB_PREFIX:-/opt/homebrew}"
    ;;
  "list --formula --versions "*)
    [ -z "${STUB_LIST_FAILS:-}" ] || failure
    shift 3
    for f in "$@"; do
      case "$f" in
        */*/*)
          echo "Error: No available formula with the name \"$f\"." >&2
          exit 1
          ;;
      esac
    done
    out="" status=0
    for f in "$@"; do
      line="$(installed "$(canonical "$f")")"
      if [ -n "$line" ]; then
        out="$out$line
"
      else
        status=1
      fi
    done
    printf %s "$out" | sort -u
    [ "$status" -ne 0 ] || [ -z "${STUB_WARNS:-}" ] || echo "Warning: stub warning" >&2
    exit "$status"
    ;;
  "deps --union "*)
    [ -z "${STUB_DEPS_FAILS:-}" ] || failure
    shift 2
    deps "$@"
    ;;
  "deps --installed --union "*)
    [ -z "${STUB_DEPS_FAILS:-}" ] || failure
    shift 3
    for d in $(deps "$@"); do
      [ -z "$(installed "$d")" ] || outdated "$d" || echo "$d"
    done
    ;;
  *) echo "stub brew: unexpected: $*" >&2; exit 64 ;;
esac
EOF
chmod +x "$stubs/uname" "$stubs/brew"
nobrew="$tmp/nobrew"
mkdir -p "$nobrew"
cp "$stubs/uname" "$nobrew/"
export PATH="$stubs:$PATH"
# The script sets these itself; a test that it does needs them unset here.
unset HOMEBREW_NO_AUTO_UPDATE HOMEBREW_NO_ENV_HINTS

# fresh_state: the installed formulae below; opencv depends on protobuf,
# which is not installed, and pkg-config is an alias of pkgconf.
fresh_state() {
  rm -rf "$BREW_STATE"
  mkdir -p "$BREW_STATE"
  printf '%s\n' 'abseil 20250814.1' 'ffmpeg 8.0_1' 'glm 1.0.3' 'libvpx 1.15.2' \
    'opencv 5.0.0' 'pkgconf 3.0.8' 'unrelated 2.0' 'x265 4.1' >"$BREW_STATE/versions"
  printf '%s\n' 'ffmpeg libvpx x265' 'opencv abseil ffmpeg libvpx protobuf x265' \
    >"$BREW_STATE/deps"
  echo 'pkg-config pkgconf' >"$BREW_STATE/aliases"
  : >"$BREW_STATE/outdated"
  : >"$CALLS"
  : >"$ENVS"
  unset STUB_OS STUB_PREFIX STUB_DEPS_FAILS STUB_LIST_FAILS STUB_PREFIX_FAILS STUB_WARNS
}

# installs NAME VERSION...: NAME is installed at those versions, in that
# order, in place of what was.
installs() {
  local name="$1"
  shift
  { grep -v "^$name " "$BREW_STATE/versions" || true; } >"$tmp/versions"
  echo "$name $*" >>"$tmp/versions"
  mv "$tmp/versions" "$BREW_STATE/versions"
}

# key ARG...: runs the script; its output in $out, its stderr in $err and its
# exit status in $status.
key() {
  status=0
  "$BASH" "$script" "$@" >"$tmp/out" 2>"$tmp/err" || status=$?
  out="$(cat "$tmp/out")"
  err="$(cat "$tmp/err")"
}

is_key() {
  if printf '%s\n' "$1" | grep -Eqx '[0-9a-f]{16}'; then echo yes; else echo "no: $1"; fi
}

# --- Arguments --------------------------------------------------------------

fresh_state
for args in "" "--verbose" "--bogus glm" "glm --verbose"; do
  # shellcheck disable=SC2086 # the arguments are split on purpose
  key $args
  expect "homebrew-key.sh rejects '$args'" "$status" 2
done
key "opencv glm"
expect "it rejects formulae passed as one word" "$status" 2

# --- Where there is no key --------------------------------------------------

fresh_state
STUB_OS=Linux key opencv
expect "off macOS it prints none and asks brew nothing" \
  "$status $out $(wc -l <"$CALLS" | tr -d ' ')" "0 none 0"
status=0
out="$(PATH="$nobrew:/usr/bin:/bin" "$BASH" "$script" opencv)" || status=$?
expect "without brew on PATH it prints none" "$status $out" "0 none"

# --- The key ----------------------------------------------------------------

fresh_state
key opencv glm
expect "it prints 16 hex digits" "$status $(is_key "$out")" "0 yes"
base="$out"
key opencv glm
expect "unchanged versions give the same key" "$out" "$base"
key glm opencv glm
expect "the formulae's order and repeats do not change it" "$out" "$base"

key --verbose opencv glm
listing='abseil 20250814.1
ffmpeg 8.0_1
glm 1.0.3
libvpx 1.15.2
opencv 5.0.0
prefix /opt/homebrew
x265 4.1'
expect "it hashes the prefix and the versions of the formulae and their installed dependencies, sorted" \
  "$err" "$listing"
hash="$(printf '%s\n' "$listing" | { sha256sum 2>/dev/null || shasum -a 256; } | cut -c1-16)"
expect "--verbose prints the same key, the listing's SHA-256" "$out $out" "$base $hash"

installs opencv 5.0.0_1
key opencv glm
expect "a formula's new version changes the key" "$(is_key "$out") $([ "$out" != "$base" ] && echo changed)" "yes changed"
installs opencv 5.0.0
key opencv glm
expect "its old version gives the old key back" "$out" "$base"

installs x265 4.2
key opencv glm
expect "a recursive dependency's new version changes it" \
  "$([ "$out" != "$base" ] && echo changed)" changed
fresh_state
installs protobuf 33.0
key opencv glm
expect "a dependency newly installed changes it" "$([ "$out" != "$base" ] && echo changed)" changed

fresh_state
installs unrelated 3.0
key opencv glm
expect "a formula outside the closure does not change it" "$out" "$base"

fresh_state
installs x265 4.1 4.2
key opencv glm
two="$out"
installs x265 4.2 4.1
key opencv glm
expect "a second installed version changes it, whatever order brew lists them in" \
  "$([ "$two" != "$base" ] && echo changed) $out" "changed $two"

fresh_state
STUB_PREFIX=/usr/local key opencv glm
expect "another prefix changes it" "$([ "$out" != "$base" ] && echo changed)" changed

# brew update makes a dependency outdated without changing what is installed.
fresh_state
echo x265 >"$BREW_STATE/outdated"
key opencv glm
expect "an outdated dependency stays in the key" "$out" "$base"
installs x265 4.2
key opencv glm
expect "a dependency's new version changes the key while it is outdated" \
  "$([ "$out" != "$base" ] && echo changed)" changed

# --- Formulae not installed, and aliases --------------------------------------

fresh_state
key --verbose opencv glm hello
expect "a formula not installed still gives a key" "$status $(is_key "$out")" "0 yes"
expect "it is hashed as not installed, and the key differs" \
  "$(printf '%s\n' "$err" | grep -c '^hello (not installed)$') $([ "$out" != "$base" ] && echo changed)" \
  "1 changed"
missing="$out"
installs hello 2.12
key opencv glm hello
expect "installing it changes the key again" \
  "$([ "$out" != "$missing" ] && [ "$out" != "$base" ] && echo changed)" changed

fresh_state
key pkgconf
direct="$out"
key --verbose pkg-config
expect "an alias gives its formula's key, and is not taken for one not installed" \
  "$out $(printf '%s\n' "$err" | grep -c 'not installed')" "$direct 0"

# --- Failures and what brew is asked ----------------------------------------

for failing in STUB_LIST_FAILS STUB_DEPS_FAILS STUB_PREFIX_FAILS; do
  fresh_state
  export "$failing=1"
  key opencv glm
  expect "a failing brew ($failing) fails it, with no key, and shows brew's error" \
    "$status $out $(printf '%s\n' "$err" | grep -c '^Error: stub failure$')" "1  1"
done
fresh_state
key glm opencv foo/bar/baz
expect "a name brew cannot resolve fails it, with no key, rather than leaving the others out" \
  "$status $out $(printf '%s\n' "$err" | grep -c 'foo/bar/baz')" "1  1"
fresh_state
STUB_WARNS=1 key opencv glm
expect "a warning from brew leaves the key as it was, and is shown" \
  "$status $out $(printf '%s\n' "$err" | grep -q '^Warning: stub warning$' && echo shown)" \
  "0 $base shown"
fresh_state
key opencv glm hello pkg-config
expect "brew is only asked to read" \
  "$(grep -c -v -E '^brew (--prefix|list --formula --versions .+|deps --union .+)$' "$CALLS")" 0
expect "with auto-update and hints off" "$(sort -u "$ENVS")" "1 1"

# --- This host's brew -------------------------------------------------------

export PATH="$real_path"
if [ "$(uname -s)" = Darwin ] && command -v brew >/dev/null 2>&1; then
  # The script's own settings, so these calls do not update Homebrew either.
  export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_ENV_HINTS=1
  # An installed formula with runtime dependencies, if there is one: a line
  # "formula: dependency...".
  installed_formulae="$(brew list --formula -1)"
  # shellcheck disable=SC2086 # one word per formula
  line="$([ -z "$installed_formulae" ] || brew deps --for-each $installed_formulae || true)"
  line="$(printf '%s\n' "$line" | awk '/: ./ && !n { print; n = 1 }')"
  formula="${line%%:*}"
  formula="${formula:-$(printf '%s\n' "$installed_formulae" | head -n 1)}"
  key --verbose "${formula:-hello}"
  first="$out" listed="$err"
  key "${formula:-hello}"
  expect "with this host's brew ($(brew --version | head -n 1), ${formula:-no formula installed}) it gives one key twice" \
    "$status $(is_key "$first") $out" "0 yes $first"
  expect "and hashes this host's prefix" \
    "$(printf '%s\n' "$listed" | grep -c "^prefix $(brew --prefix)$")" 1
  if [ -n "$line" ]; then
    wanted=0 found=0
    # shellcheck disable=SC2086 # one word per formula
    for dep in $({ brew list --formula --versions ${line#*: } || true; } | cut -d' ' -f1); do
      wanted=$((wanted + 1))
      [ -z "$(printf '%s\n' "$listed" | awk -v n="$dep" '$1 == n')" ] || found=$((found + 1))
    done
    expect "and the versions of the $wanted installed dependencies of $formula" \
      "$found $([ "$wanted" -gt 0 ] && echo some)" "$wanted some"
  else
    echo "skip this host's dependencies: no installed formula has any"
  fi
else
  echo "skip this host's brew: not macOS, or no brew"
fi

report
