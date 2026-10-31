# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
"""Tests of check_private.py against a registry the test answers for: GHCR's
answers to an anonymous client for a private and a public package, and the
answers it cannot read."""

import contextlib
import io
import json
import os
import sys
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import check_private  # noqa: E402

PACKAGE = "taojin-6/volumetric_kit_ci"
IMAGE = f"ghcr.io/{PACKAGE}:ubuntu-26.04"
TOKEN_URL = f"https://ghcr.io/token?scope=repository:{PACKAGE}:pull"
MANIFEST_URL = f"https://ghcr.io/v2/{PACKAGE}/manifests/ubuntu-26.04"
DENIED = json.dumps({"errors": [{"code": "DENIED"}]}).encode()


class Registry:
    """Answers TOKEN_URL with TOKEN and MANIFEST_URL with MANIFEST, each
    (status, body), and records each request."""

    def __init__(self, token, manifest=(500, b"")):
        self.answers = {TOKEN_URL: token, MANIFEST_URL: manifest}
        self.requests = []

    def __call__(self, url, headers):
        self.requests.append((url, dict(headers)))
        return self.answers[url]


def anonymous_token():
    return 200, json.dumps({"token": "anon"}).encode()


def run(registry, argv=(IMAGE,), actions=False):
    out = io.StringIO()
    env = {"GITHUB_ACTIONS": "true"} if actions else {}
    with mock.patch.dict(os.environ, env, clear=True), contextlib.redirect_stdout(out):
        code = check_private.main(list(argv), get=registry)
    return code, out.getvalue()


class VerdictTest(unittest.TestCase):
    def test_a_refused_token_is_private(self):
        # What GHCR answers today for a package an anonymous client may not read.
        registry = Registry(token=(403, DENIED))
        self.assertEqual(run(registry)[0], 0)
        self.assertEqual([url for url, _ in registry.requests], [TOKEN_URL])

    def test_a_refused_manifest_is_private(self):
        for status in (401, 403, 404):
            registry = Registry(anonymous_token(), manifest=(status, DENIED))
            self.assertEqual(run(registry)[0], 0, status)

    def test_a_served_manifest_is_public(self):
        registry = Registry(anonymous_token(), manifest=(200, b"{}"))
        code, out = run(registry, actions=True)
        self.assertEqual(code, 1)
        self.assertTrue(out.startswith("::error::"), out)
        self.assertIn(f"the package {PACKAGE} is public", out)
        self.assertIn("Set it to private", out)

    def test_the_requests_carry_no_credentials(self):
        registry = Registry(anonymous_token(), manifest=(200, b"{}"))
        run(registry)
        (token_url, token_headers), (manifest_url, manifest_headers) = registry.requests
        self.assertEqual((token_url, token_headers), (TOKEN_URL, {}))
        self.assertEqual(manifest_url, MANIFEST_URL)
        self.assertEqual(manifest_headers["Authorization"], "Bearer anon")
        self.assertIn("application/vnd.oci.image.index.v1+json", manifest_headers["Accept"])


class CannotTellTest(unittest.TestCase):
    def assert_fails(self, registry, fragment):
        code, out = run(registry)
        self.assertEqual(code, 1)
        self.assertIn("cannot tell whether the image is private", out)
        self.assertIn(fragment, out)

    def test_a_failed_token_request(self):
        self.assert_fails(Registry(token=(503, b"")), "token request returned 503")

    def test_a_token_answer_without_a_token(self):
        self.assert_fails(Registry(token=(200, b"<html>")), "holds no token")
        self.assert_fails(Registry(token=(200, b"{}")), "holds no token")

    def test_a_failed_manifest_fetch(self):
        self.assert_fails(Registry(anonymous_token(), manifest=(502, b"")),
                          "manifest fetch returned 502")

    def test_no_connection(self):
        def unreachable(url, headers):
            raise OSError("no route to host")
        self.assert_fails(unreachable, "no route to host")


class UsageTest(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(check_private.parse(IMAGE), (PACKAGE, "ubuntu-26.04"))
        for reference in (f"docker.io/{PACKAGE}:ubuntu-26.04", f"ghcr.io/{PACKAGE}",
                          "ghcr.io/volumetric_kit_ci:ubuntu-26.04", f"ghcr.io/{PACKAGE}:",
                          f"ghcr.io/{PACKAGE}@sha256:" + "0" * 64):
            self.assertIsNone(check_private.parse(reference), reference)

    def test_a_bad_reference_asks_nothing(self):
        registry = Registry(token=(403, DENIED))
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(run(registry, argv=("ubuntu:26.04",))[0], 2)
            self.assertEqual(run(registry, argv=())[0], 2)
        self.assertEqual(registry.requests, [])


if __name__ == "__main__":
    unittest.main()
