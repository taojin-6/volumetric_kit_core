#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
#
# Runs homebrew-key.sh against a stub brew that lists a fake set of installed
# formulae, and checks when its key changes and when it does not. Where brew
# itself is installed on macOS, it also runs the script against that, which
# only reads. The script runs under the bash running this, so
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
# "name dependency..." per formula, its dependencies, recursively, installed
# or not; `aliases` a line "alias name" per alias. It logs every call, prints
# $STUB_PREFIX (default /opt/homebrew) for --prefix, and fails `deps` when
# STUB_DEPS_FAILS is set. `uname -s` names $STUB_OS (default Darwin).
stubs="$tmp/stubs"
mkdir -p "$stubs"
export CALLS="$tmp/calls.log" BREW_STATE="$tmp/state"
printf '%s\n' '#!/bin/sh' 'echo "${STUB_OS:-Darwin}"' >"$stubs/uname"
cat >"$stubs/brew" <<'EOF'
#!/bin/sh
echo "brew $*" >>"$CALLS"
canonical() {
  awk -v a="$1" '$1 == a { n = $2 } END { print (n == "" ? a : n) }' "$BREW_STATE/aliases"
}
installed() { awk -v n="$1" '$1 == n' "$BREW_STATE/versions"; }
case "$*" in
  --prefix) echo "${STUB_PREFIX:-/opt/homebrew}" ;;
  "list --formula --versions "*)
    shift 3
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
    exit "$status"
    ;;
  "deps --installed --union "*)
    [ -z "${STUB_DEPS_FAILS:-}" ] || { echo "Error: stub failure" >&2; exit 1; }
    shift 3
    for f in "$@"; do
      for d in $(awk -v n="$(canonical "$f")" '$1 == n { $1 = ""; print }' "$BREW_STATE/deps"); do
        [ -z "$(installed "$d")" ] || echo "$d"
      done
    done | sort -u
    ;;
  *) echo "stub brew: unexpected: $*" >&2; exit 64 ;;
esac
EOF
chmod +x "$stubs/uname" "$stubs/brew"
nobrew="$tmp/nobrew"
mkdir -p "$nobrew"
cp "$stubs/uname" "$nobrew/"
export PATH="$stubs:$PATH"

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
  : >"$CALLS"
  unset STUB_OS STUB_PREFIX STUB_DEPS_FAILS
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

fresh_state
STUB_DEPS_FAILS=1 key opencv glm
expect "a failing brew deps fails it, with no key" "$status $out" "1 "
fresh_state
key opencv glm hello pkg-config
expect "brew is only asked to read" \
  "$(grep -c -v -E '^brew (--prefix|list --formula --versions .+|deps --installed --union .+)$' "$CALLS")" 0

# --- This host's brew -------------------------------------------------------

export PATH="$real_path"
if [ "$(uname -s)" = Darwin ] && command -v brew >/dev/null 2>&1; then
  formula="$(brew list --formula -1 | head -n 1)"
  key --verbose "${formula:-hello}"
  first="$out" listed="$err"
  key "${formula:-hello}"
  expect "with this host's brew ($(brew --version | head -n 1), ${formula:-no formula installed}) it gives one key twice" \
    "$status $(is_key "$first") $out" "0 yes $first"
  expect "and hashes this host's prefix" \
    "$(printf '%s\n' "$listed" | grep -c "^prefix $(brew --prefix)$")" 1
else
  echo "skip this host's brew: not macOS, or no brew"
fi

report
