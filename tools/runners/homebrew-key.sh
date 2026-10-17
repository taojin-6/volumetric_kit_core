#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
#
# Prints a cache key for the Homebrew formulae a build uses: 16 hex digits
# hashed from Homebrew's prefix and the installed versions of those formulae
# and of all they depend on, recursively, so a brew upgrade that changes any
# of them changes the key. Off macOS, or without brew on PATH, it prints
# "none". It only reads what is installed, with Homebrew's auto-update off.
# README.md says how CI uses it.
#
#   bash tools/runners/homebrew-key.sh [--verbose] FORMULA...
#
# --verbose also prints what it hashes, to stderr. A formula that is not
# installed is hashed as such, so installing it changes the key too. When
# brew fails, or cannot resolve a name, it prints no key and exits 1.
set -euo pipefail

usage() {
  echo "usage: $0 [--verbose] FORMULA..." >&2
  exit 2
}

verbose=0
if [ "${1:-}" = --verbose ]; then
  verbose=1
  shift
fi
[ $# -gt 0 ] || usage
for formula in "$@"; do
  case "$formula" in -* | "" | *[[:space:]]*) usage ;; esac
done

if [ "$(uname -s)" != Darwin ] || ! command -v brew >/dev/null 2>&1; then
  echo none
  exit 0
fi

# Read only: no auto-update, not even the download of Homebrew's metadata
# that brew otherwise makes once its copy is a week old.
export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_ENV_HINTS=1

# Names and versions are words, never patterns.
set -f

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# installed FORMULA...: sets $lines to a line "name version..." for each that
# is installed, under its own name (not an alias), its versions sorted.
installed() {
  local status=0 name kegs
  lines=""
  brew list --formula --versions "$@" >"$tmp/list" 2>"$tmp/err" || status=$?
  # brew exits 1, saying nothing, when a formula is not installed; the loop
  # below finds those. It failed if it exits 1 saying something, as for a
  # name it cannot resolve (it then lists none of the others), or exits
  # with more; what it says when it succeeds is a warning, passed on.
  if [ "$status" -gt 1 ] || { [ "$status" -eq 1 ] && [ -s "$tmp/err" ]; }; then
    echo "error: brew list failed" >&2
    cat "$tmp/err" >&2
    exit 1
  fi
  cat "$tmp/err" >&2
  while read -r name kegs; do
    # shellcheck disable=SC2086 # one word per version
    kegs="$(printf '%s\n' $kegs | LC_ALL=C sort | tr '\n' ' ')"
    lines="$lines$name ${kegs% }
"
  done <"$tmp/list"
}

prefix="$(brew --prefix)" && [ -n "$prefix" ] || {
  echo "error: brew --prefix failed" >&2
  exit 1
}

installed "$@"
found="$lines"
names=" $(printf '%s' "$found" | cut -d' ' -f1 | tr '\n' ' ')"
missing=""
for formula in "$@"; do
  case "$names" in *" $formula "*) continue ;; esac
  # Listed under another name, as an alias is, or not installed.
  installed "$formula"
  [ -n "$lines" ] || missing="$missing$formula (not installed)
"
done

if [ -n "$found" ]; then
  # Their runtime dependencies, recursively, as their kegs record them.
  # Not --installed: it leaves out a dependency that is outdated (older
  # than Homebrew's metadata, as brew update leaves it). installed drops
  # one that is no longer installed.
  # shellcheck disable=SC2046 # one word per formula
  deps="$(brew deps --union $(printf '%s' "$found" | cut -d' ' -f1))" || {
    echo "error: brew deps failed" >&2
    exit 1
  }
  if [ -n "$deps" ]; then
    # shellcheck disable=SC2086 # one word per formula
    installed $deps
    found="$found$lines"
  fi
fi

listing="$(
  {
    echo "prefix $prefix"
    printf '%s' "$found" "$missing"
  } | sed '/^$/d' | LC_ALL=C sort -u
)"

[ "$verbose" -eq 0 ] || printf '%s\n' "$listing" >&2
if command -v sha256sum >/dev/null 2>&1; then
  printf '%s\n' "$listing" | sha256sum | cut -c1-16
else
  printf '%s\n' "$listing" | shasum -a 256 | cut -c1-16
fi
