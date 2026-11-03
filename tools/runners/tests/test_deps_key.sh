#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
#
# Runs deps-key.sh against stub tools (uname, the compilers, macOS's
# sw_vers, xcode-select and xcrun, and dpkg-query), and checks which part of
# the prefix each input changes and when it fails. Then it runs the script
# against this host's own tools, which it only reads. The script runs under
# the bash running this, so `/bin/bash test_deps_key.sh` tests it under
# macOS's bash 3.2.
# shellcheck disable=SC2016 # the stubs' variables expand when the stubs run
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
script="$here/../deps-key.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
# shellcheck source-path=SCRIPTDIR source=lib.sh
. "$here/lib.sh"
real_path="$PATH"

# --- Stubs ------------------------------------------------------------------

# `uname -s` names $STUB_OS (default Linux), `uname -m` $STUB_ARCH (default
# x86_64). c++ and clang++ print a version from $STUB_CXX and $STUB_CLANG,
# and c++ fails when STUB_CXX_FAILS is set. sw_vers, xcode-select and xcrun
# print $STUB_MACOS, $STUB_XCODE and $STUB_SDK; xcrun fails when
# STUB_XCRUN_FAILS is. dpkg-query answers from $DPKG, a line "name version"
# per installed package and "name" for one dpkg knows but has not
# installed: like the real one it lists, in name order, the packages it
# knows, and fails naming each it does not. Every call is logged to $CALLS.
stubs="$tmp/stubs"
mkdir -p "$stubs"
export CALLS="$tmp/calls.log" DPKG="$tmp/dpkg"
cat >"$stubs/uname" <<'EOF'
#!/bin/sh
case "$1" in
  -s) echo "${STUB_OS:-Linux}" ;;
  -m) echo "${STUB_ARCH:-x86_64}" ;;
  *) echo "stub uname: unexpected: $*" >&2; exit 64 ;;
esac
EOF
cat >"$stubs/c++" <<'EOF'
#!/bin/sh
echo "c++ $*" >>"$CALLS"
[ -z "${STUB_CXX_FAILS:-}" ] || { echo "c++: stub failure" >&2; exit 1; }
printf 'c++ (Stub) %s\nCopyright stub\n' "${STUB_CXX:-13.3.0}"
EOF
cat >"$stubs/clang++" <<'EOF'
#!/bin/sh
printf 'clang version %s\nTarget: stub\n' "${STUB_CLANG:-21.1.0}"
EOF
printf '%s\n' '#!/bin/sh' 'echo "sw_vers $*" >>"$CALLS"' 'echo "${STUB_MACOS:-26.1}"' \
  >"$stubs/sw_vers"
printf '%s\n' '#!/bin/sh' 'echo "${STUB_XCODE:-/Applications/Xcode.app/Contents/Developer}"' \
  >"$stubs/xcode-select"
cat >"$stubs/xcrun" <<'EOF'
#!/bin/sh
[ -z "${STUB_XCRUN_FAILS:-}" ] || { echo "xcrun: stub failure" >&2; exit 1; }
echo "${STUB_SDK:-26.1}"
EOF
cat >"$stubs/dpkg-query" <<'EOF'
#!/bin/sh
echo "dpkg-query $*" >>"$CALLS"
[ "$1" = -W ] && [ "$2" = '-f=${Package} ${Version}\n' ] || {
  echo "stub dpkg-query: unexpected: $*" >&2
  exit 64
}
shift 2
status=0
for p in "$@"; do
  awk -v n="$p" '$1 == n { found = 1 } END { exit !found }' "$DPKG" || {
    echo "dpkg-query: no packages found matching $p" >&2
    status=1
  }
done
for p in "$@"; do awk -v n="$p" '$1 == n { print $1 " " $2 }' "$DPKG"; done | sort -u
exit "$status"
EOF
chmod +x "$stubs"/*
export PATH="$stubs:$PATH"

# A PATH with no dpkg-query: the stubs but it, and the tools the script uses.
nodpkg="$tmp/nodpkg"
mkdir -p "$nodpkg"
for tool in uname c++ sw_vers xcode-select xcrun; do cp "$stubs/$tool" "$nodpkg/"; done
for tool in awk cat cut grep mktemp rm sort sha256sum shasum perl; do
  found="$(PATH="$real_path" command -v "$tool" || true)"
  [ -z "$found" ] || ln -s "$found" "$nodpkg/$tool"
done

# fresh: the default stubs' answers, and these packages.
fresh() {
  printf '%s\n' 'cmake 3.31.6-1' 'libc6-dev 2.42-0ubuntu3' 'libvulkan-dev 1.4.321-1' \
    'libgone' >"$DPKG"
  : >"$CALLS"
  unset STUB_OS STUB_ARCH STUB_CXX STUB_CLANG STUB_CXX_FAILS STUB_MACOS STUB_XCODE \
    STUB_SDK STUB_XCRUN_FAILS
}

# key ARG...: runs the script; its output in $out, its stderr in $err and
# its exit status in $status.
key() {
  status=0
  "$BASH" "$script" "$@" >"$tmp/out" 2>"$tmp/err" || status=$?
  out="$(cat "$tmp/out")"
  err="$(cat "$tmp/err")"
}

hash12() { { sha256sum 2>/dev/null || shasum -a 256; } | cut -c1-12; }

# part NAME PREFIX: the brew, tc or ws part of a prefix.
part() {
  case "$1" in
    brew) printf '%s\n' "$2" | sed -n 's/^brew\([^-]*\)-tc.*/\1/p' ;;
    tc) printf '%s\n' "$2" | sed -n 's/.*-tc\([0-9a-f]*\)-ws.*/\1/p' ;;
    ws) printf '%s\n' "$2" | sed -n 's/.*-ws\([0-9a-f]*\)$/\1/p' ;;
  esac
}

# changed PREFIX: which of brew, tc and ws differ from $base's.
changed() {
  local p which=""
  for p in brew tc ws; do
    [ "$(part "$p" "$1")" = "$(part "$p" "$base")" ] || which="$which $p"
  done
  echo "${which# }"
}

is_prefix() {
  if printf '%s\n' "$1" | grep -Eqx 'brew(none|[0-9a-f]{16})-tc[0-9a-f]{12}-ws[0-9a-f]{12}'; then
    echo yes
  else
    echo "no: $1"
  fi
}

ws=/w/repo/repo
brewkey=0123456789abcdef

# --- Arguments --------------------------------------------------------------

fresh
for args in "" "--verbose" "--workspace" "--workspace $ws --bogus" \
  "--workspace $ws --brew" "--workspace $ws --brew xyz" \
  "--workspace $ws --brew 0123456789ABCDEF" "--workspace $ws --brew ${brewkey}0" \
  "--workspace $ws cmake --verbose" "--workspace $ws -- cmake"; do
  # shellcheck disable=SC2086 # the arguments are split on purpose
  key $args
  expect "deps-key.sh rejects '$args'" "$status $out" "2 "
done
key --workspace "" cmake
expect "it rejects an empty workspace" "$status" 2
key --workspace "$ws" --compiler "" cmake
expect "it rejects an empty compiler" "$status" 2
key --workspace "$ws" "cmake libc6-dev"
expect "it rejects packages passed as one word" "$status" 2

# --- The prefix -------------------------------------------------------------

fresh
key --workspace "$ws" cmake libc6-dev
expect "it prints brewnone-tc<12 hex>-ws<12 hex>" \
  "$status $(is_prefix "$out") $(part brew "$out")" "0 yes none"
base="$out"
key --workspace "$ws" cmake libc6-dev
expect "the same inputs give the same prefix" "$out" "$base"
key --workspace "$ws" --brew "" libc6-dev cmake cmake
expect "an empty Homebrew key is none, and the packages' order and repeats change nothing" \
  "$out" "$base"
key --workspace "$ws" --brew none cmake libc6-dev
expect "so is none" "$out" "$base"
key --workspace "$ws" --brew "$brewkey" cmake libc6-dev
expect "a Homebrew key is taken whole, and changes nothing else" \
  "$(part brew "$out") $(changed "$out")" "$brewkey brew"
expect "the workspace part is the SHA-256 of its path" \
  "$(part ws "$base")" "$(printf '%s' "$ws" | hash12)"

key --verbose --workspace "$ws" --container '{"image":"ubuntu@sha256:00"}' cmake libc6-dev
listing="workspace $ws
arch x86_64
c++ (Stub) 13.3.0
Copyright stub
cmake 3.31.6-1
libc6-dev 2.42-0ubuntu3
container {\"image\":\"ubuntu@sha256:00\"}"
expect "--verbose prints what it hashes: the machine, the compiler, the packages and the container" \
  "$err" "$listing"
expect "and the toolchain part is that listing's SHA-256, past the workspace line" \
  "$(part tc "$out")" "$(printf '%s\n' "$listing" | sed 1d | hash12)"

# --- Each input moves its own part ------------------------------------------

fresh
key --workspace /w/other/other cmake libc6-dev
expect "another workspace changes the workspace part alone" "$(changed "$out")" ws
STUB_CXX=14.2.0 key --workspace "$ws" cmake libc6-dev
expect "another compiler version changes the toolchain part alone" "$(changed "$out")" tc
key --workspace "$ws" --compiler clang++ cmake libc6-dev
expect "another compiler changes it" "$(changed "$out")" tc
STUB_ARCH=aarch64 key --workspace "$ws" cmake libc6-dev
expect "another machine changes it" "$(changed "$out")" tc
key --workspace "$ws" --container '{"image":"ubuntu@sha256:00"}' cmake libc6-dev
container="$out"
expect "a container changes it" "$(changed "$out")" tc
key --workspace "$ws" --container '{"image":"ubuntu@sha256:01"}' cmake libc6-dev
expect "and another container again" "$([ "$out" != "$container" ] && changed "$out")" tc
printf '%s\n' 'cmake 3.31.6-2' 'libc6-dev 2.42-0ubuntu3' >"$DPKG"
key --workspace "$ws" cmake libc6-dev
expect "a package's new version changes it" "$(changed "$out")" tc
fresh
key --workspace "$ws" cmake libc6-dev libvulkan-dev
expect "another package changes it" "$(changed "$out")" tc
fresh
key --workspace "$ws"
expect "with no packages dpkg-query is not asked" \
  "$status $(is_prefix "$out") $(grep -c '^dpkg-query' "$CALLS" || true)" "0 yes 0"

# macOS: its versions, not the packages.
fresh
export STUB_OS=Darwin
key --workspace "$ws" cmake libc6-dev
mac="$out"
expect "on macOS it gives a prefix, without asking dpkg-query" \
  "$status $(is_prefix "$out") $(grep -c '^dpkg-query' "$CALLS" || true) $([ "$out" != "$base" ] && echo differs)" \
  "0 yes 0 differs"
key --workspace "$ws"
expect "where the packages change nothing" "$out" "$mac"
base="$mac"
STUB_MACOS=26.2 key --workspace "$ws"
expect "another macOS changes the toolchain part" "$(changed "$out")" tc
STUB_XCODE=/Applications/Xcode-beta.app/Contents/Developer key --workspace "$ws"
expect "another Xcode changes it" "$(changed "$out")" tc
STUB_SDK=26.2 key --workspace "$ws"
expect "another SDK changes it" "$(changed "$out")" tc
key --verbose --workspace "$ws"
expect "on macOS --verbose lists the macOS, Xcode and SDK versions" \
  "$(printf '%s\n' "$err" | grep -E '^(macos|xcode|sdk) ')" \
  "macos 26.1
xcode /Applications/Xcode.app/Contents/Developer
sdk 26.1"

# --- Failures ---------------------------------------------------------------

fresh
key --workspace "$ws" cmake libgone libvulkan-dev
expect "a package dpkg knows but has not installed fails it, printing nothing, naming it" \
  "$status|$out|$err" "1||error: not installed: libgone"
key --workspace "$ws" cmake nosuch libc6-dev
expect "an unknown package fails it, printing nothing, with dpkg-query's error" \
  "$status|$out|$(printf '%s\n' "$err" | grep -c 'nosuch')" "1||2"
status=0
PATH="$nodpkg" "$BASH" "$script" --workspace "$ws" cmake >"$tmp/out" 2>"$tmp/err" || status=$?
expect "packages without dpkg-query on PATH fail it, printing nothing, naming them" \
  "$status|$(cat "$tmp/out")|$(cat "$tmp/err")" \
  "1||error: dpkg-query is not on PATH, so these packages' versions cannot be read: cmake"
status=0
out="$(PATH="$nodpkg" "$BASH" "$script" --workspace "$ws")" || status=$?
expect "no packages need no dpkg-query" "$status $(is_prefix "$out")" "0 yes"
STUB_CXX_FAILS=1 key --workspace "$ws" cmake
expect "a failing compiler fails it, printing nothing, with its error" \
  "$status|$out|$(printf '%s\n' "$err" | grep -c -e 'stub failure' -e 'c++ --version failed')" "1||2"
key --workspace "$ws" --compiler no-such-c++ cmake
expect "so does a compiler not on PATH" "$status|$out" "1|"
STUB_OS=Darwin STUB_XCRUN_FAILS=1 key --workspace "$ws"
expect "a failing macOS tool fails it, printing nothing" \
  "$status|$out|$(printf '%s\n' "$err" | grep -c 'xcrun --show-sdk-version failed')" "1||1"

# --- This host's tools --------------------------------------------------------

export PATH="$real_path"
fresh
packages=""
if [ "$(uname -s)" != Darwin ] && command -v dpkg-query >/dev/null 2>&1; then
  # A package every host that compiles C++ has.
  packages="$(dpkg-query -W -f='${Package}\n' 'libc6-dev' 2>/dev/null || true)"
fi
# shellcheck disable=SC2086 # zero or one package
key --verbose --workspace "$PWD" $packages
first="$out"
# shellcheck disable=SC2086 # zero or one package
key --workspace "$PWD" $packages
expect "with this host's tools ($(uname -s) $(uname -m)${packages:+, $packages}) it gives one prefix twice" \
  "$status $(is_prefix "$first") $out" "0 yes $first"

report
