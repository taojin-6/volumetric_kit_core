# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
"""Tests of check_dockerfiles.py: the family's images pass, and each rule
fails a copy of one that breaks it."""

import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import check_dockerfiles  # noqa: E402

VERSIONS = ("24.04", "26.04")


def image(version):
    with open(os.path.join(HERE, f"ubuntu-{version}", "Dockerfile"), encoding="utf-8") as file:
        return file.read()


def broken(version, old, new):
    """The ubuntu-VERSION Dockerfile with OLD, which occurs once, as NEW."""
    text = image(version)
    if text.count(old) != 1:
        raise AssertionError(f"{old!r} occurs {text.count(old)} times in ubuntu-{version}")
    return text.replace(old, new)


def messages(text, version):
    return [message for _, message in check_dockerfiles.check_text(text, version)]


class FamilyImagesTest(unittest.TestCase):
    def test_the_family_images_pass(self):
        problems, count = check_dockerfiles.check(HERE)
        self.assertEqual(problems, [])
        self.assertEqual(count, len(VERSIONS))

    def test_main_reports_them_clean(self):
        self.assertEqual(check_dockerfiles.main([HERE]), 0)

    def test_a_directory_without_images_fails(self):
        with tempfile.TemporaryDirectory() as empty:
            self.assertEqual(check_dockerfiles.main([empty]), 1)


class RulesTest(unittest.TestCase):
    def assert_refused(self, text, version, fragment):
        found = messages(text, version)
        self.assertTrue(any(fragment in message for message in found),
                        f"expected {fragment!r} among {found}")

    def test_a_tag_instead_of_a_digest(self):
        for version in VERSIONS:
            digest = next(line for line in image(version).splitlines()
                          if line.startswith("FROM "))
            text = broken(version, digest, f"FROM ubuntu:{version}")
            self.assert_refused(text, version, "pin ubuntu by digest")

    def test_a_comment_naming_another_tag(self):
        text = broken("26.04", "# ubuntu:26.04\nFROM", "# ubuntu:24.04\nFROM")
        self.assert_refused(text, "26.04", "# ubuntu:26.04")

    def test_a_second_stage(self):
        text = image("24.04") + "\nFROM ubuntu@sha256:" + "0" * 64 + "\n"
        self.assert_refused(text, "24.04", "2 FROM instructions")

    def test_recommends(self):
        text = broken("24.04", "apt-get install -y --no-install-recommends \\\n      ca-certificates",
                      "apt-get install -y \\\n      ca-certificates")
        self.assert_refused(text, "24.04", "without --no-install-recommends")

    def test_lists_left_in_the_layer(self):
        text = broken("26.04", "portaudio19-dev ffmpeg \\\n      clang libclang-rt-dev llvm "
                      "mesa-vulkan-drivers \\\n && rm -rf /var/lib/apt/lists/*",
                      "portaudio19-dev ffmpeg \\\n      clang libclang-rt-dev llvm "
                      "mesa-vulkan-drivers")
        self.assert_refused(text, "26.04", "must end by removing /var/lib/apt/lists/*")

    def test_an_unchecked_deb(self):
        text = broken("24.04", ' \\\n && echo "${keyring_sha256}  /tmp/cuda-keyring.deb" | sha256sum -c -',
                      "")
        self.assert_refused(text, "24.04", "dpkg -i of a file whose SHA256")

    def test_no_source_label(self):
        text = broken("26.04", "volumetric_kit_core\"", "volumetric_kit_recon\"")
        self.assert_refused(text, "26.04", "org.opencontainers.image.source")

    def test_a_comment_inside_a_run_is_skipped(self):
        text = broken("24.04", "      ca-certificates git sudo curl gnupg \\\n",
                      "      ca-certificates git sudo curl gnupg \\\n# a note\n")
        self.assertEqual(messages(text, "24.04"), [])


if __name__ == "__main__":
    unittest.main()
