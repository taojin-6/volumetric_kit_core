#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
"""Fail on a workflow trigger an outsider can cause, or a schedule.

Reads each workflow file, *.yml and *.yaml, of one directory, finds the events
under its top-level `on:` in any of YAML's forms -- a name, a list or a map,
in block or flow style -- and names each refused one with its file and line.
A file whose triggers it cannot read fails too. It needs Python's standard
library alone: no YAML parser is installed where it runs.
"""

import os
import re
import sys

# The refused events, and why. Each but the schedule runs without the owner's
# push or pull request, and the first two with a token that can write.
REFUSED = {
    "pull_request_target": "it runs this repository's workflow, with its secrets and a "
                           "token that can write, on a fork's pull request",
    "workflow_run": "it runs with this repository's secrets and a token that can write "
                    "after a run that a fork's pull request can start",
    "issue_comment": "anyone can trigger it with a comment",
    "pull_request_review_comment": "anyone can trigger it with a review comment",
    "discussion": "anyone can trigger it with a discussion",
    "discussion_comment": "anyone can trigger it with a comment on a discussion",
    "schedule": "the family runs no scheduled jobs: the owner triggers every run",
}

# A top-level `on` key, plain or quoted, and the rest of its line.
ON_KEY = re.compile(r"""^(?:on|"on"|'on')[ \t]*:(?=[ \t]|$)(.*)$""")
PLAIN_KEY_END = re.compile(r":(?=[ \t]|$)")


class Unreadable(Exception):
    """A file whose triggers cannot be read: the line, and why."""

    def __init__(self, line, why):
        super().__init__(why)
        self.line = line


def quote_end(text):
    """The index of the quote that closes the one TEXT starts with, or None."""
    i = 1
    while i < len(text):
        if text[0] == '"' and text[i] == "\\":
            i += 2
            continue
        if text[i] == text[0]:
            if text[0] == "'" and text[i + 1:i + 2] == "'":
                i += 2
                continue
            return i
        i += 1
    return None


def strip_comment(text):
    """TEXT without a trailing comment: a # at its start or after a space or
    tab, outside quotes."""
    i = 0
    while i < len(text):
        c = text[i]
        if c in "'\"" and (i == 0 or text[i - 1] in " \t[{,:-"):
            end = quote_end(text[i:])
            if end is None:
                return text.rstrip()
            i += end + 1
            continue
        if c == "#" and (i == 0 or text[i - 1] in " \t"):
            return text[:i].rstrip()
        i += 1
    return text.rstrip()


def scalar(text, line):
    """The name a scalar TEXT spells, quoted or plain."""
    text = text.strip()
    if text[:1] in ("'", '"') and quote_end(text) == len(text) - 1:
        if text[0] == '"' and "\\" in text:
            raise Unreadable(line, f"an escape in the quoted name {text}")
        return text[1:-1].replace("''", "'") if text[0] == "'" else text[1:-1]
    if not text or text[0] in "[]{}&*!|>%@`'\",?:-#":
        raise Unreadable(line, f"cannot read the name {text!r}")
    return text


def split_key(text, line):
    """The key of a map entry TEXT, `key: value` or a quoted key and `:`, or
    None when TEXT is no map entry."""
    text = text.strip()
    if text[:1] in ("'", '"'):
        end = quote_end(text)
        if end is None:
            raise Unreadable(line, f"a quote that is never closed in {text!r}")
        if text[end + 1:].lstrip().startswith(":"):
            return scalar(text[:end + 1], line)
        return None
    match = PLAIN_KEY_END.search(text)
    return scalar(text[:match.start()], line) if match else None


def entry_event(entry, row):
    """The event a flow entry ENTRY on line ROW (0-based) names, as a list:
    `name` or `name: value`, and none if ENTRY is empty."""
    entry = entry.strip()
    if not entry:
        return []
    key = split_key(entry, row + 1)
    if key is None and entry[0] not in "'\"" and ":" in entry:
        key = scalar(entry.split(":", 1)[0], row + 1)  # `name:value`
    return [(key if key is not None else scalar(entry, row + 1), row + 1)]


def flow_events(lines, start, text):
    """The events of the flow collection, [..] or {..}, that TEXT on line
    START (0-based) of LINES opens; it may run over several lines."""
    events = []
    depth = 0
    entry = ""
    entry_row = row = start
    while True:
        i = 0
        while i < len(text):
            c = text[i]
            if depth == 1 and not entry.strip() and c not in " \t,]}":
                entry_row = row
            if c in "'\"":
                end = quote_end(text[i:])
                if end is None:
                    raise Unreadable(row + 1, "a quote that is never closed under on:")
                entry += text[i:i + end + 1]
                i += end + 1
                continue
            if c in "[{":
                depth += 1
                if depth > 1:
                    entry += c
            elif c in "]}":
                depth -= 1
                if depth > 0:
                    entry += c
                else:
                    if text[i + 1:].strip():
                        raise Unreadable(row + 1, "text after the bracket that closes on:")
                    return events + entry_event(entry, entry_row)
            elif c == "," and depth == 1:
                events += entry_event(entry, entry_row)
                entry = ""
            else:
                entry += c
            i += 1
        row += 1
        if row >= len(lines):
            raise Unreadable(start + 1, "on: opens a bracket that is never closed")
        text = strip_comment(lines[row])
        entry += " "


def indent_of(text):
    return len(text) - len(text.lstrip(" "))


def block_events(lines, start):
    """The events of the block under `on:` on line START (0-based): a list of
    names, or a map whose keys are events."""
    rows = [r for r in range(start + 1, len(lines)) if strip_comment(lines[r]).strip()]
    first = strip_comment(lines[rows[0]]) if rows else ""
    indent = indent_of(first)
    is_list = first.strip() == "-" or first.strip().startswith("- ")
    if not rows or (indent == 0 and not is_list):
        raise Unreadable(start + 1, "on: names no event")
    events = []
    for r in rows:
        text = strip_comment(lines[r])
        here = indent_of(text)
        body = text.strip()
        if here < indent or (here == 0 and not (body == "-" or body.startswith("- "))):
            break
        if here > indent:
            continue
        if is_list:
            if not (body == "-" or body.startswith("- ")):
                raise Unreadable(r + 1, "on: mixes a list and a map")
            item = body[1:].strip()
            if not item:
                raise Unreadable(r + 1, "an empty list item under on:")
            key = split_key(item, r + 1)
            events.append((key if key is not None else scalar(item, r + 1), r + 1))
        else:
            key = split_key(body, r + 1)
            if key is None:
                raise Unreadable(r + 1, f"cannot read {body!r} under on:")
            events.append((key, r + 1))
    return events


def triggers(text):
    """The events that trigger the workflow whose file holds TEXT, each with
    its line: [(name, line)]. Raises Unreadable."""
    lines = [line.rstrip("\r") for line in text.split("\n")]
    events = []
    found = False
    for row, line in enumerate(lines):
        match = ON_KEY.match(line)
        if not match:
            continue
        found = True
        value = strip_comment(match.group(1)).strip()
        if not value:
            events += block_events(lines, row)
        elif value[0] in "[{":
            events += flow_events(lines, row, value)
        else:
            events.append((scalar(value, row + 1), row + 1))
    if not found:
        raise Unreadable(1, "no top-level on:")
    return events


def check(directory):
    """The refusals in the workflows of DIRECTORY, each (path, line, message),
    and how many workflows it read."""
    names = sorted(name for name in os.listdir(directory)
                   if name.endswith((".yml", ".yaml"))
                   and os.path.isfile(os.path.join(directory, name)))
    refusals = []
    for name in names:
        path = os.path.join(directory, name)
        with open(path, encoding="utf-8") as file:
            text = file.read()
        try:
            events = triggers(text)
        except Unreadable as error:
            refusals.append((path, error.line, f"cannot read its triggers: {error}"))
            continue
        for event, line in events:
            why = REFUSED.get(event.lower())
            if why:
                refusals.append((path, line, f"on: {event} is refused: {why}"))
    return refusals, len(names)


def main(argv=None):
    args = sys.argv[1:] if argv is None else argv
    if len(args) != 1:
        print("usage: check_workflow_triggers.py <workflows directory>", file=sys.stderr)
        return 2
    directory = args[0]
    if not os.path.isdir(directory):
        print(f"no directory {directory}: check the repository out first", file=sys.stderr)
        return 1
    refusals, count = check(directory)
    if count == 0:
        print(f"no workflow in {directory}", file=sys.stderr)
        return 1
    annotate = os.environ.get("GITHUB_ACTIONS") == "true"
    for path, line, message in refusals:
        path = os.path.relpath(path)
        print(f"::error file={path},line={line}::{message}" if annotate
              else f"{path}:{line}: {message}")
    print(f"{len(refusals)} refused in {count} workflows of {directory}")
    return 1 if refusals else 0


if __name__ == "__main__":
    sys.exit(main())
