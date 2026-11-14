# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import io
import json
import os
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

    def test_unpack_keeps_private_files_and_writes_version_last(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(update, "ROOT", Path(folder).resolve()):
            root = update.ROOT
            (root / "private").mkdir()
            (root / "private" / "launcher.json").write_text("mine")
            (root / "Launcher.exe").write_text("old")
            update.unpack(archive({"DOA2U-2/VERSION": "2", "DOA2U-2/Launcher.exe": "new", "DOA2U-2/tools/a.py": "a",
                                   "DOA2U-2/private/launcher.json": "release", "DOA2U-2/private/photo.jpg": "photo"}))
            self.assertEqual((root / "Launcher.exe").read_text(), "new")
            self.assertEqual((root / "Launcher.exe.old").read_text(), "old")
            self.assertEqual((root / "tools" / "a.py").read_text(), "a")
            self.assertEqual((root / "private" / "launcher.json").read_text(), "mine")
            self.assertEqual((root / "private" / "photo.jpg").read_text(), "photo")
            # A failed write puts the running exe back and leaves VERSION, which goes last, for a retry.
            real_write = Path.write_bytes

            def fail_on_exe(path, data):
                if path.name == "Launcher.exe":
                    raise OSError("disk full")
                return real_write(path, data)

            with mock.patch.object(Path, "write_bytes", fail_on_exe):
                with self.assertRaises(OSError):
                    update.unpack(archive({"DOA2U-3/VERSION": "3", "DOA2U-3/tools/a.py": "b", "DOA2U-3/Launcher.exe": "x"}))
            self.assertEqual((root / "tools" / "a.py").read_text(), "b")
            self.assertEqual((root / "VERSION").read_text(), "2")
            self.assertEqual((root / "Launcher.exe").read_text(), "new")

    def test_unpack_spares_the_running_bundled_python(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(update, "ROOT", Path(folder).resolve()), \
                mock.patch.object(update.sys, "platform", "win32"):
            python = update.ROOT / "tools" / "python"
            (python / "DLLs").mkdir(parents=True)
            (python / "python.exe").write_text("same")
            (python / "DLLs" / "_ssl.pyd").write_text("old")
            real_write = Path.write_bytes

            def locked(path, data):
                if path.exists() and python in path.parents:
                    raise PermissionError("in use")
                return real_write(path, data)

            with mock.patch.object(Path, "write_bytes", locked):
                update.unpack(archive({"DOA2U-2/tools/python/python.exe": "same",
                                       "DOA2U-2/tools/python/DLLs/_ssl.pyd": "new"}))
            self.assertEqual((python / "DLLs" / "_ssl.pyd").read_text(), "new")
            self.assertEqual((python / "DLLs" / "_ssl.pyd.old").read_text(), "old")
            self.assertFalse((python / "python.exe.old").exists())

    def test_unpack_reapplies_a_changed_mode_outside_windows(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(update, "ROOT", Path(folder).resolve()), \
                mock.patch.object(update.sys, "platform", "linux"), mock.patch.object(Path, "chmod") as chmod:
            (update.ROOT / "build_game.sh").write_text("same")
            data = io.BytesIO()
            with zipfile.ZipFile(data, "w") as writer:
                info = zipfile.ZipInfo("DOA2U-2/build_game.sh")
                info.external_attr = 0o100755 << 16
                writer.writestr(info, "same")
            update.unpack(zipfile.ZipFile(data))
            chmod.assert_called_once_with(0o755)

    def test_install_downloads_only_changed_entries(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as writer:
            writer.writestr("DOA2U-2/big.bin", os.urandom(1 << 21))  # random, so it stays 2 MB compressed
            writer.writestr("DOA2U-2/VERSION", "2")
        zipped = data.getvalue()
        with zipfile.ZipFile(io.BytesIO(zipped)) as source:
            big = source.read("DOA2U-2/big.bin")
            hashes = {info.filename: [hashlib.sha256(source.read(info)).hexdigest(), (info.external_attr >> 16) & 0o777]
                      for info in source.infolist()}
        asset = {"name": "DOA2U-2.zip", "browser_download_url": "zip", "size": len(zipped), "digest": "sha256:" + "0" * 64}

        def manifest(listed):
            body = json.dumps(listed).encode()
            return body, {"name": "DOA2U-2.zip.sha256.json", "browser_download_url": "manifest",
                          "digest": "sha256:" + hashlib.sha256(body).hexdigest()}

        # (case, ranges served, manifest listing, expected error); the ZIP's own digest is wrong on purpose,
        # so any whole-ZIP download fails its checksum and shows which path ran.
        cases = (("changed only", True, hashes, None), ("no ranges", False, hashes, "GitHub's checksum"),
                 ("no manifest", True, None, "GitHub's checksum"),
                 ("tampered entry", True, {**hashes, "DOA2U-2/VERSION": ["0" * 64, 0]}, "file list"),
                 # The ZIP's directory says big.bin matches the install, but the manifest lists newer bytes: the
                 # manifest decides, so big.bin (most of the ZIP) is pending, the whole ZIP downloads and is refused.
                 ("stale directory", True, {**hashes, "DOA2U-2/big.bin": ["0" * 64, 0]}, "GitHub's checksum"),
                 ("entry missing from the list", True, {"DOA2U-2/VERSION": hashes["DOA2U-2/VERSION"]}, "files do not"))
        self.assertEqual(update.manifest_of([manifest(hashes)[1], asset], asset), manifest(hashes)[1])
        for case, ranges, listed, error in cases:
            with self.subTest(case), tempfile.TemporaryDirectory() as folder, \
                    mock.patch.object(update, "ROOT", Path(folder).resolve()), \
                    mock.patch.object(update.Ranges, "BLOCK", 4096):
                (update.ROOT / "big.bin").write_bytes(big)
                body, listing = manifest(listed) if listed else (b"", None)
                served = []

                def fetch(url, start=None, end=None):
                    if url == "manifest":
                        response = io.BytesIO(body)
                    else:
                        response = io.BytesIO(zipped[start:end] if start is not None and ranges else zipped)
                        served.append(len(response.getvalue()))
                    response.status = 206 if start is not None and ranges else 200
                    return response

                with mock.patch.object(update, "fetch", fetch):
                    if error is None:
                        update.install(asset, listing)
                        self.assertEqual((update.ROOT / "VERSION").read_text(), "2")
                        self.assertLess(sum(served), len(zipped) // 10)
                    else:
                        with self.assertRaisesRegex(ValueError, error):
                            update.install(asset, listing)
                        self.assertFalse((update.ROOT / "VERSION").exists())

    def test_size_estimate_counts_a_read_ahead_block_per_scattered_entry(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as writer:
            writer.writestr("DOA2U-2/a", "a")
            writer.writestr("DOA2U-2/mid.bin", os.urandom(1 << 16))
            writer.writestr("DOA2U-2/b", "b")
        with zipfile.ZipFile(data) as archive, mock.patch.object(update.Ranges, "BLOCK", 4096):
            entries = archive.infolist()
            self.assertEqual(update.ranged_bytes([(entries[0], None, 0), (entries[2], None, 0)]), 2 * 4096)
            self.assertEqual(update.ranged_bytes([(entries[0], None, 0)]), 4096)

    def test_unsafe_entry_stops_before_any_write(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(update, "ROOT", Path(folder).resolve()):
            with self.assertRaises(ValueError):
                update.unpack(archive({"DOA2U-2/tools/a.py": "a", "DOA2U-2/../../evil": "x"}))
            self.assertFalse((update.ROOT / "tools").exists())


if __name__ == "__main__":
    unittest.main()
