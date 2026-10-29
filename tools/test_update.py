# SPDX-License-Identifier: GPL-3.0-or-later
import io
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import zipfile

import update


def archive(files):
    data = io.BytesIO()
    with zipfile.ZipFile(data, "w") as zipped:
        for name, content in files.items():
            zipped.writestr(name, content)
    return zipfile.ZipFile(data)


class UpdateTest(unittest.TestCase):
    def test_versions_compare_numerically(self):
        self.assertGreater(update.number("v0.1.10"), update.number("0.1.9-Alpha"))
        self.assertEqual(update.number("0.1.10"), update.number("0.1.10-Alpha"))

    def test_asset_matches_the_platform(self):
        assets = [{"name": "DOA2U-1-Alpha-Linux.zip"}, {"name": "DOA2U-1-Alpha-macOS.zip"}, {"name": "DOA2U-1-Alpha.zip"}]
        for suffix, expected in (("", "DOA2U-1-Alpha.zip"), ("-macOS", "DOA2U-1-Alpha-macOS.zip"), ("-Linux", "DOA2U-1-Alpha-Linux.zip")):
            with mock.patch.object(update, "SUFFIX", suffix):
                self.assertEqual(update.pick_asset(assets)["name"], expected)

    def test_unpack_writes_below_the_root_and_keeps_private_files(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(update, "ROOT", Path(folder).resolve()):
            root = update.ROOT
            (root / "private").mkdir()
            (root / "private" / "launcher.json").write_text("mine")
            (root / "Launcher.exe").write_text("old")
            update.unpack(archive({"DOA2U-2/Launcher.exe": "new", "DOA2U-2/tools/a.py": "a", "DOA2U-2/VERSION": "2"}))
            self.assertEqual((root / "Launcher.exe").read_text(), "new")
            self.assertEqual((root / "Launcher.exe.old").read_text(), "old")
            self.assertEqual((root / "tools" / "a.py").read_text(), "a")
            self.assertEqual((root / "private" / "launcher.json").read_text(), "mine")
            with self.assertRaises(ValueError):
                update.unpack(archive({"DOA2U-2/../../evil": "x"}))


if __name__ == "__main__":
    unittest.main()
