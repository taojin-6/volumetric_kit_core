#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
"""Fail while anyone can pull a CI image.

Asks GHCR, with no credentials, for a pull token for the image's package and
then for the tag's manifest. The images carry NVIDIA's CUDA and TensorRT,
which are not ours to redistribute publicly, so a manifest an anonymous
client can read fails the check (README.md, "The owner's one-time step").
Python's standard library alone, as in tools/github.
"""

import json
import os
import sys
import urllib.error
import urllib.request

REGISTRY = "ghcr.io"
ACCEPT = ", ".join((
    "application/vnd.oci.image.index.v1+json",
    "application/vnd.oci.image.manifest.v1+json",
    "application/vnd.docker.distribution.manifest.list.v2+json",
    "application/vnd.docker.distribution.manifest.v2+json",
))
# What GHCR answers an anonymous client for a package it may not read. A 404
# is not one: a public package answers it for a tag it lacks.
REFUSED = (401, 403)


def fetch(url, headers):
    """(status, body) of a GET of URL with HEADERS alone, no credentials."""
    request = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as error:
        return error.code, error.read()


def parse(reference):
    """(package, tag) of a ghcr.io/<owner>/<name>:<tag> REFERENCE, else None."""
    registry, _, rest = reference.partition("/")
    package, _, tag = rest.rpartition(":")
    if registry != REGISTRY or "/" not in package or "@" in package or not tag or "/" in tag:
        return None
    return package, tag


def check(package, tag, get=fetch):
    """(verdict, message) for PACKAGE:TAG: "private" when GHCR refuses an
    anonymous pull, "public" when it serves the manifest, None for any other
    answer. GET(url, headers) returns (status, body)."""
    status, body = get(f"https://{REGISTRY}/token?scope=repository:{package}:pull", {})
    if status in REFUSED:
        return "private", f"{package}: an anonymous pull token is refused ({status})"
    if status != 200:
        return None, f"{package}: the anonymous token request returned {status}"
    try:
        token = json.loads(body)["token"]
    except (ValueError, KeyError, TypeError):
        return None, f"{package}: the anonymous token answer holds no token"
    status, _ = get(f"https://{REGISTRY}/v2/{package}/manifests/{tag}",
                    {"Authorization": f"Bearer {token}", "Accept": ACCEPT})
    if status in REFUSED:
        return "private", f"{package}:{tag}: an anonymous manifest fetch is refused ({status})"
    if status == 200:
        return "public", (
            f"the package {package} is public: anyone can pull {tag}. Set it to private "
            "(README.md, \"The owner's one-time step\"): the images carry NVIDIA's "
            "components, which are not ours to redistribute publicly")
    return None, f"{package}:{tag}: the anonymous manifest fetch returned {status}"


def main(argv=None, get=fetch):
    args = sys.argv[1:] if argv is None else argv
    parsed = parse(args[0]) if len(args) == 1 else None
    if parsed is None:
        print("usage: check_private.py ghcr.io/<owner>/<name>:<tag>", file=sys.stderr)
        return 2
    try:
        verdict, message = check(*parsed, get=get)
    except OSError as error:
        verdict, message = None, f"{args[0]}: {error}"
    if verdict == "private":
        print(message)
        return 0
    if verdict is None:
        message = f"cannot tell whether the image is private: {message}"
    annotate = os.environ.get("GITHUB_ACTIONS") == "true"
    print(f"::error::{message}" if annotate else message)
    return 1


if __name__ == "__main__":
    sys.exit(main())
