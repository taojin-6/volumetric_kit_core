# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
"""Tests of check_dockerfiles.py: the family's images pass, and each rule
fails a copy of one that breaks it."""

import hashlib
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

    def test_the_24_04_image_carries_the_decklink_sdk(self):
        # Where sensor's DeckLink driver finds the SDK (its vs_decklink.cmake:
        # DeckLinkSDK_ROOT/include, DeckLinkAPIDispatch.cpp beside the headers).
        text = image("24.04")
        self.assertIn("\nCOPY decklink-sdk-16.0/include /opt/decklink-sdk-16.0/include\n", text)
        self.assertIn("\nENV DeckLinkSDK_ROOT=/opt/decklink-sdk-16.0\n", text)
        for name in ("DeckLinkAPI.h", "DeckLinkAPIDispatch.cpp", "DeckLinkAPIVersion.h"):
            self.assertTrue(os.path.isfile(os.path.join(SDK, "include", name)), name)
        with open(os.path.join(SDK, "include", "DeckLinkAPIVersion.h"), encoding="utf-8") as file:
            self.assertIn('BLACKMAGIC_DECKLINK_API_VERSION_STRING\t\t\t"16.0"', file.read())


SDK = os.path.join(HERE, "ubuntu-24.04", "decklink-sdk-16.0")


class VendoredSdkTest(unittest.TestCase):
    """The DeckLink SDK's files stay as Blackmagic ships them."""

    def listed(self):
        with open(os.path.join(SDK, "SHA256SUMS"), encoding="utf-8") as file:
            rows = [line.split("  ", 1) for line in file.read().splitlines() if line]
        return {path: digest for digest, path in rows}

    def test_every_file_is_listed_and_unchanged(self):
        listed = self.listed()
        present = sorted("include/" + name for name in os.listdir(os.path.join(SDK, "include")))
        self.assertEqual(sorted(listed), present)
        for path, digest in listed.items():
            with open(os.path.join(SDK, path), "rb") as file:
                self.assertEqual(hashlib.sha256(file.read()).hexdigest(), digest, path)

    def test_every_file_keeps_its_licence_statement(self):
        for path in self.listed():
            with open(os.path.join(SDK, path), encoding="utf-8") as file:
                head = file.read(4096)
            self.assertIn("-LICENSE-START-", head, path)
            self.assertIn("Blackmagic Design", head, path)


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

    def test_add(self):
        text = broken("24.04", "\nCOPY decklink-sdk-16.0/include", "\nADD decklink-sdk-16.0/include")
        self.assert_refused(text, "24.04", "ADD fetches and unpacks")

    def test_a_copy_of_a_path_not_beside_the_dockerfile(self):
        text = broken("24.04", "COPY decklink-sdk-16.0/include", "COPY decklink-sdk-15.3/include")
        found = [message for _, message in check_dockerfiles.check_text(
            text, "24.04", os.path.join(HERE, "ubuntu-24.04"))]
        self.assertIn("COPY decklink-sdk-15.3/include: not in the image's directory", found)

    def test_a_copy_from_outside_the_directory(self):
        for source in ("../smoke.sh", "/etc/hosts", "decklink-sdk-16.0/*"):
            text = broken("24.04", "COPY decklink-sdk-16.0/include", f"COPY {source}")
            self.assert_refused(text, "24.04", f"COPY {source}: a path inside")

    def test_a_copy_from_another_stage(self):
        text = broken("24.04", "COPY decklink-sdk-16.0/include",
                      "COPY --from=builder decklink-sdk-16.0/include")
        self.assert_refused(text, "24.04", "COPY --from")

    def test_a_comment_inside_a_run_is_skipped(self):
        text = broken("24.04", "      ca-certificates git sudo curl gnupg \\\n",
                      "      ca-certificates git sudo curl gnupg \\\n# a note\n")
        self.assertEqual(messages(text, "24.04"), [])


if __name__ == "__main__":
    unittest.main()
