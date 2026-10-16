#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
#
# Prints a cache key for the Homebrew formulae a build uses: 16 hex digits
# hashed from Homebrew's prefix and the installed versions of those formulae
# and of all they depend on, recursively, so a brew upgrade that changes any
# of them changes the key. Off macOS, or without brew on PATH, it prints
# "none". It only reads what is installed. README.md says how CI uses it.
#
#   bash tools/runners/homebrew-key.sh [--verbose] FORMULA...
#
# --verbose also prints what it hashes, to stderr. A formula that is not
# installed is hashed as such, so installing it changes the key too.
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

# Names and versions are words, never patterns.
set -f

# installed FORMULA...: "name version..." for each that is installed, under
# its own name (not an alias), its versions sorted. brew exits 1 when one is
# not installed; the loop below finds those.
installed() {
  { brew list --formula --versions "$@" || true; } | while read -r name kegs; do
    # shellcheck disable=SC2046,SC2086 # one word per version
    echo "$name" $(printf '%s\n' $kegs | LC_ALL=C sort)
  done
}

given="$(installed "$@")"
names=" $(printf '%s\n' "$given" | cut -d' ' -f1 | tr '\n' ' ')"
missing=""
for formula in "$@"; do
  case "$names" in *" $formula "*) continue ;; esac
  # Listed under another name, as an alias is, or not installed.
  [ -n "$(installed "$formula")" ] || missing="$missing$formula (not installed)
"
done

deps=""
if [ -n "$given" ]; then
  # shellcheck disable=SC2046 # one word per formula
  deps="$(brew deps --installed --union $(printf '%s\n' "$given" | cut -d' ' -f1))" ||
    {
      echo "error: brew deps failed" >&2
      exit 1
    }
fi

listing="$(
  {
    echo "prefix $(brew --prefix)"
    printf '%s\n' "$given"
    # shellcheck disable=SC2086 # one word per formula
    [ -z "$deps" ] || installed $deps
    printf '%s' "$missing"
  } | sed '/^$/d' | LC_ALL=C sort -u
)"

[ "$verbose" -eq 0 ] || printf '%s\n' "$listing" >&2
if command -v sha256sum >/dev/null 2>&1; then
  printf '%s\n' "$listing" | sha256sum | cut -c1-16
else
  printf '%s\n' "$listing" | shasum -a 256 | cut -c1-16
fi
