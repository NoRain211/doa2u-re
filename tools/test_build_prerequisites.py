# SPDX-License-Identifier: GPL-3.0-or-later
"""Compiler download checks without network access."""
# Adapted from committed DOAXBV tools/test_build_prerequisites.py at 5761e89.
import hashlib
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import urllib.error
import zipfile

import build_prerequisites as prerequisites


class PrerequisiteTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.download = self.enterContext(mock.patch.object(
            prerequisites.urllib.request, "urlopen", side_effect=AssertionError("Unexpected download")))
        self.enterContext(mock.patch("builtins.print"))
        self.enterContext(mock.patch.dict(os.environ, PATH="system-tools"))

    def llvm_mingw_zip(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as archive:
            archive.writestr("llvm-mingw-test-ucrt-x86_64/bin/clang.exe", b"clang")
        return data.getvalue()

    def test_download_installs_verified_toolchain_once_and_leads_path(self):
        payload = self.llvm_mingw_zip()
        self.download.side_effect = lambda *args, **kwargs: io.BytesIO(payload)
        with mock.patch.object(prerequisites, "LLVM_MINGW_SHA256", hashlib.sha256(payload).hexdigest()):
            clang = prerequisites.llvm_mingw(self.root)
            self.assertEqual(prerequisites.llvm_mingw(self.root), clang)
        self.assertEqual(clang.read_bytes(), b"clang")
        self.assertEqual(list((self.root / "private").iterdir()), [])
        self.download.assert_called_once_with(prerequisites.LLVM_MINGW_URL, timeout=60)
        self.assertEqual(os.environ["PATH"], os.pathsep.join([str(clang.parent), "system-tools"]))

    def test_failed_download_or_hash_installs_nothing(self):
        payload = self.llvm_mingw_zip()
        for failure, message in ((urllib.error.URLError("offline"), "Could not download"),
                                 (None, "SHA-256")):
            with self.subTest(message=message):
                self.download.side_effect = failure or (lambda *args, **kwargs: io.BytesIO(payload))
                with self.assertRaisesRegex(ValueError, message):
                    prerequisites.llvm_mingw(self.root)
                self.assertFalse((self.root / "tools/llvm-mingw").exists())
                self.assertEqual(list((self.root / "private").iterdir()), [])

    def extract_xiso_zip(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as archive:
            archive.writestr("LICENSE.TXT", b"license")
            archive.writestr("extract-xiso", b"tool")
        return data.getvalue()

    def test_extract_xiso_is_fetched_once_on_linux_without_one_on_path(self):
        payload = self.extract_xiso_zip()
        self.download.side_effect = lambda *args, **kwargs: io.BytesIO(payload)
        with mock.patch.object(prerequisites, "runs_upstream_linux_binary", return_value=True), \
                mock.patch.object(prerequisites.shutil, "which", return_value=None), \
                mock.patch.object(prerequisites, "EXTRACT_XISO_SHA256", hashlib.sha256(payload).hexdigest()):
            tool = prerequisites.extract_xiso(self.root)
            self.assertEqual(prerequisites.extract_xiso(self.root), tool)
            self.assertEqual(tool, self.root / f"private/extract-xiso-{prerequisites.EXTRACT_XISO_SHA256[:12]}")
        self.assertEqual(tool.read_bytes(), b"tool")
        self.assertEqual([p.name for p in (self.root / "private").iterdir()], [tool.name])
        self.download.assert_called_once_with(prerequisites.EXTRACT_XISO_URL, timeout=60)

    def test_extract_xiso_defers_to_path_and_other_hosts(self):
        for on_linux, on_path in ((True, "/usr/bin/extract-xiso"), (False, None)):
            with self.subTest(on_linux=on_linux), \
                    mock.patch.object(prerequisites, "runs_upstream_linux_binary", return_value=on_linux), \
                    mock.patch.object(prerequisites.shutil, "which", return_value=on_path):
                self.assertEqual(prerequisites.extract_xiso(self.root), "extract-xiso")
        self.assertFalse((self.root / "private").exists())

    def test_upstream_binary_needs_x86_64_glibc_2_34(self):
        for machine, libc, expected in (("x86_64", ("glibc", "2.39"), True), ("x86_64", ("glibc", "2.31"), False),
                                        ("x86_64", ("", ""), False), ("aarch64", ("glibc", "2.39"), False)):
            with self.subTest(machine=machine, libc=libc), \
                    mock.patch.object(prerequisites.sys, "platform", "linux"), \
                    mock.patch.object(prerequisites.platform, "machine", return_value=machine), \
                    mock.patch.object(prerequisites.platform, "libc_ver", return_value=libc):
                self.assertEqual(prerequisites.runs_upstream_linux_binary(), expected)

    def test_extract_xiso_hash_mismatch_installs_nothing(self):
        self.download.side_effect = lambda *args, **kwargs: io.BytesIO(self.extract_xiso_zip())
        with mock.patch.object(prerequisites, "runs_upstream_linux_binary", return_value=True), \
                mock.patch.object(prerequisites.shutil, "which", return_value=None):
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                prerequisites.extract_xiso(self.root)
        self.assertEqual(list((self.root / "private").iterdir()), [])


if __name__ == "__main__":
    unittest.main()
