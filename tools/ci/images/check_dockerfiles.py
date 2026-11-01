#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
"""Fail on a CI image Dockerfile that breaks the family's rules.

Reads each ubuntu-<version>/Dockerfile under a directory and names, with its
file and line, each rule an instruction breaks (README.md, "The rules").
Python's standard library alone, as in tools/github.
"""

import json
import os
import re
import sys

SOURCE = "https://github.com/taojin-6/volumetric_kit_core"
DIGEST = re.compile(r"^ubuntu@sha256:[0-9a-f]{64}$")
VERSION_DIR = re.compile(r"^ubuntu-(\d\d\.\d\d)$")


def instructions(text):
    """The instructions of a Dockerfile TEXT, each (line, keyword, arguments,
    comment above): continuation lines joined, comments inside one dropped."""
    result = []
    lines = text.split("\n")
    row = 0
    comment = None
    while row < len(lines):
        line = lines[row].strip()
        row += 1
        if not line:
            comment = None
            continue
        if line.startswith("#"):
            comment = line
            continue
        start = row
        parts = []
        while line.endswith("\\") and row < len(lines):
            parts.append(line[:-1].strip())
            line = lines[row].strip()
            row += 1
            while line.startswith("#") and row < len(lines):
                line = lines[row].strip()
                row += 1
        parts.append(line)
        joined = " ".join(part for part in parts if part)
        keyword, _, arguments = joined.partition(" ")
        result.append((start, keyword.upper(), arguments.strip(), comment))
        comment = None
    return result


def copy_problems(arguments, directory):
    """What breaks the COPY rule in a COPY's ARGUMENTS: [message]. With
    DIRECTORY, the image's build context, each source must exist in it."""
    flags = []
    rest = arguments
    while rest.startswith("--"):
        flag, _, rest = rest.partition(" ")
        flags.append(flag)
        rest = rest.lstrip()
    problems = []
    if any(flag.startswith("--from") for flag in flags):
        problems.append("COPY --from: the image is one stage, copying only from its directory")
    if rest.startswith("<<"):
        return problems + ["COPY <<: a heredoc; COPY a file kept beside the Dockerfile"]
    if rest.startswith("["):
        try:
            paths = json.loads(rest)
        except ValueError:
            paths = None
        if not isinstance(paths, list) or not all(isinstance(path, str) for path in paths):
            return problems + [f"COPY {arguments}: not a JSON array of strings"]
    else:
        paths = rest.split()
    if len(paths) < 2:
        return problems + [f"COPY {arguments}: a source and a destination"]
    for source in paths[:-1]:
        parts = source.replace("\\", "/").split("/")
        if source.startswith("/") or ".." in parts or any(c in source for c in "*?["):
            problems.append(f"COPY {source}: a path inside the image's directory, "
                            "no '..', absolute path or wildcard")
        elif directory is not None and not os.path.exists(os.path.join(directory, source)):
            problems.append(f"COPY {source}: not in the image's directory")
    return problems


def check_text(text, version, directory=None):
    """The rules TEXT, the Dockerfile of ubuntu-VERSION, breaks: [(line, message)].
    DIRECTORY, the Dockerfile's own, is where a COPY's sources must exist."""
    problems = []
    found = instructions(text)
    froms = [entry for entry in found if entry[1] == "FROM"]
    if len(froms) != 1:
        problems.append((1, f"{len(froms)} FROM instructions; one is the rule"))
    for line, _, arguments, comment in froms:
        if not DIGEST.match(arguments):
            problems.append((line, f"FROM {arguments}: pin ubuntu by digest, ubuntu@sha256:<64 hex>"))
        if comment != f"# ubuntu:{version}":
            problems.append((line, f"the comment above FROM must name the tag it pins, # ubuntu:{version}"))
    labels = " ".join(arguments for _, keyword, arguments, _ in found if keyword == "LABEL")
    if f'org.opencontainers.image.source="{SOURCE}"' not in labels:
        problems.append((1, f'no LABEL org.opencontainers.image.source="{SOURCE}"'))
    for line, keyword, arguments, _ in found:
        if keyword == "ADD":
            problems.append((line, "ADD fetches and unpacks; COPY a file kept beside the "
                                   "Dockerfile instead"))
        if keyword == "COPY":
            problems += [(line, message) for message in copy_problems(arguments, directory)]
        if keyword != "RUN":
            continue
        for install in re.findall(r"apt-get install[^&;|]*", arguments):
            if "--no-install-recommends" not in install:
                problems.append((line, "apt-get install without --no-install-recommends"))
        if "apt-get update" in arguments and not arguments.rstrip().endswith(
                "rm -rf /var/lib/apt/lists/*"):
            problems.append((line, "a RUN that runs apt-get update must end by removing "
                                   "/var/lib/apt/lists/*"))
        dpkg = arguments.find("dpkg -i")
        if dpkg >= 0 and "sha256sum -c" not in arguments[:dpkg]:
            problems.append((line, "dpkg -i of a file whose SHA256 the RUN has not checked"))
    return problems


def check(directory):
    """The problems of every ubuntu-<version>/Dockerfile under DIRECTORY, each
    (path, line, message), and how many Dockerfiles it read."""
    problems = []
    count = 0
    for name in sorted(os.listdir(directory)):
        match = VERSION_DIR.match(name)
        path = os.path.join(directory, name, "Dockerfile")
        if not match or not os.path.isfile(path):
            continue
        count += 1
        with open(path, encoding="utf-8") as file:
            text = file.read()
        problems += [(path, line, message) for line, message in
                     check_text(text, match.group(1), os.path.join(directory, name))]
    return problems, count


def main(argv=None):
    args = sys.argv[1:] if argv is None else argv
    if len(args) != 1:
        print("usage: check_dockerfiles.py <images directory>", file=sys.stderr)
        return 2
    problems, count = check(args[0])
    if count == 0:
        print(f"no ubuntu-<version>/Dockerfile in {args[0]}", file=sys.stderr)
        return 1
    annotate = os.environ.get("GITHUB_ACTIONS") == "true"
    for path, line, message in problems:
        path = os.path.relpath(path)
        print(f"::error file={path},line={line}::{message}" if annotate
              else f"{path}:{line}: {message}")
    print(f"{len(problems)} problems in {count} Dockerfiles of {args[0]}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
