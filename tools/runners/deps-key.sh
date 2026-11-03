#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
#
# Prints the prefix of a build's dependency-cache key,
# brew<KEY>-tc<TOOLCHAIN>-ws<WORKSPACE>, which names what a cached
# FetchContent tree was built with beyond its pins:
#
# - KEY, what homebrew-key.sh printed for the leg's formulae, or none;
# - TOOLCHAIN, 12 hex digits hashed from the machine, the compiler's
#   --version, on macOS the macOS, Xcode and SDK versions, on Linux the
#   versions of the Debian packages named, and the job's container;
# - WORKSPACE, 12 hex digits hashed from the checkout's path, which
#   FetchContent writes into the trees it builds.
#
#   bash tools/runners/deps-key.sh [--verbose] --workspace PATH [--brew KEY]
#     [--container TEXT] [--compiler CXX] [PACKAGE...]
#
# KEY may be empty, for none, as where the Homebrew key did not run. TEXT is
# the job's container as the workflow names it, empty for none. CXX is the
# compiler the dependencies build with, c++ by default. The PACKAGEs are
# Debian packages they build against; macOS has none, its formulae being in
# KEY, so there they are not read. --verbose also prints what it hashes, to
# stderr. It prints nothing and exits 1 when the compiler, a macOS tool or
# dpkg-query fails, or a package is not installed. README.md says how CI
# uses it.
set -euo pipefail

usage() {
  echo "usage: $0 [--verbose] --workspace PATH [--brew KEY] [--container TEXT] [--compiler CXX] [PACKAGE...]" >&2
  exit 2
}

fail() {
  echo "error: $*" >&2
  exit 1
}

verbose=0
workspace=""
brew=""
container=""
compiler=c++
while [ $# -gt 0 ]; do
  case "$1" in
    --verbose)
      verbose=1
      shift
      ;;
    --workspace | --brew | --container | --compiler)
      [ $# -ge 2 ] || usage
      case "$1" in
        --workspace) workspace="$2" ;;
        --brew) brew="$2" ;;
        --container) container="$2" ;;
        --compiler) compiler="$2" ;;
      esac
      shift 2
      ;;
    -*) usage ;;
    *) break ;;
  esac
done
[ -n "$workspace" ] && [ -n "$compiler" ] || usage
for package in "$@"; do
  case "$package" in -* | "" | *[[:space:]]*) usage ;; esac
done
# The key goes into a cache key whole: only homebrew-key.sh's forms.
case "$brew" in
  "" | none) brew=none ;;
  *)
    printf '%s\n' "$brew" | grep -Eqx '[0-9a-f]{16}' || {
      echo "error: not a Homebrew key: '$brew'" >&2
      usage
    }
    ;;
esac

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# read_out CMD...: prints what CMD prints, or fails naming it.
read_out() {
  "$@" 2>"$tmp/err" || {
    cat "$tmp/err" >&2
    fail "$* failed"
  }
}

# Each part is read before any is printed, so a failure prints nothing.
arch="$(read_out uname -m)"
cxx="$(read_out "$compiler" --version)"
os="$(read_out uname -s)"
platform=""
if [ "$os" = Darwin ]; then
  macos="$(read_out sw_vers -productVersion)"
  xcode="$(read_out xcode-select -p)"
  sdk="$(read_out xcrun --show-sdk-version)"
  platform="macos $macos
xcode $xcode
sdk $sdk"
elif [ $# -gt 0 ]; then
  command -v dpkg-query >/dev/null 2>&1 ||
    fail "dpkg-query is not on PATH, so these packages' versions cannot be read: $*"
  # shellcheck disable=SC2016 # dpkg-query's own fields
  platform="$(read_out dpkg-query -W -f='${Package} ${Version}\n' "$@" | LC_ALL=C sort -u)"
  # Known to dpkg but not installed: listed with no version.
  absent="$(printf '%s\n' "$platform" | awk 'NF < 2 { printf " %s", $1 }')"
  [ -z "$absent" ] || fail "not installed:$absent"
fi

listing="arch $arch
$cxx
${platform:+$platform
}container $container"

hash12() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum | cut -c1-12
  else
    shasum -a 256 | cut -c1-12
  fi
}

toolchain="$(printf '%s\n' "$listing" | hash12)"
ws="$(printf '%s' "$workspace" | hash12)"
if [ "$verbose" -ne 0 ]; then
  printf 'workspace %s\n%s\n' "$workspace" "$listing" >&2
fi
echo "brew$brew-tc$toolchain-ws$ws"
