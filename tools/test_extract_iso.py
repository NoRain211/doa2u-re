# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic listing checks; optionally exercise a real extract-xiso binary."""
import os
import io
import json
import shutil
import sys
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from extract_iso import check_path, extract, listing_files, read_listing, run_logged, sha256


def listing(entries, count=1):
    return "extract-xiso test\nlisting synthetic.iso:\n" + entries + f"\n{count} files in synthetic.iso total 3 bytes\n"


class ExtractionTests(unittest.TestCase):
    def test_live_log_and_failed_command(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            log, gate = root / "build.log", root / "continue"

            class Console(io.StringIO):
                def write(console, text):
                    if text.strip() == "ready":
                        self.assertIn(b"ready", log.read_bytes())
                        gate.touch()
                    return super().write(text)

            script = ("import pathlib,sys,time; print('ready',flush=True); "
                      "p=pathlib.Path(sys.argv[1]); deadline=time.monotonic()+5\n"
                      "while not p.exists() and time.monotonic()<deadline: time.sleep(.01)\n"
                      "print('compiler error' if p.exists() else 'output was buffered',file=sys.stderr); "
                      "sys.exit(7)")
            console = Console()
            with patch("sys.stdout", console), self.assertRaises(subprocess.CalledProcessError) as failure:
                run_logged([sys.executable, "-u", "-c", script, str(gate)], log)
            self.assertEqual(failure.exception.returncode, 7)
            self.assertTrue(gate.exists(), "Output must reach the console before the child exits")
            self.assertIn("compiler error", console.getvalue())
            self.assertIn(b"compiler error", log.read_bytes())

    def test_paths_and_complete_listing(self):
        self.assertEqual(listing_files(listing("/sub/ (0 bytes)\n/sub/a.txt (3 bytes)")),
                         {"sub/a.txt": 3})
        for name in ("../escape", "a/../escape", "/absolute", "C:/escape", "NUL", "a:stream", "a.", "a\nname"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                listing_files(listing(f"/{name} (3 bytes)"))
        for entries, count in (("/a (3 bytes)\n/A (3 bytes)", 2),
                               ("/a (3 bytes)\n/a/b (3 bytes)", 2),
                               ("/a (3 bytes)", 2), ("/a (3 bytes)\nWARNING: truncated", 1)):
            with self.assertRaises(ValueError):
                listing_files(listing(entries, count))

    def test_listing_written_in_the_ansi_code_page(self):
        # A Japanese Windows prints the image path as cp932, which is not valid UTF-8.
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "listing.log"
            path.write_bytes(listing("/sub/a.txt (3 bytes)").replace(
                "synthetic.iso", "新しいフォルダー.iso").encode("cp932"))
            with patch("extract_iso.narrow_encoding", return_value="cp932"):
                text = read_listing(path)
            self.assertTrue(text.endswith("新しいフォルダー.iso total 3 bytes\n"))
            self.assertEqual(listing_files(text), {"sub/a.txt": 3})

    def test_paths_the_extractor_cannot_name(self):
        with patch("extract_iso.narrow_encoding", return_value="ascii"):
            check_path("C:/games/game.iso")
            with self.assertRaisesRegex(ValueError, "ascii"):
                check_path("/games/新しいフォルダー/game.iso")

    @unittest.skipUnless(os.environ.get("EXTRACT_XISO_TEST_TOOL"), "optional real extractor check")
    def test_real_extraction_and_no_overwrite(self):
        private = Path(__file__).resolve().parents[1] / "private"
        with tempfile.TemporaryDirectory(prefix="extract-test-", dir=private) as folder:
            root = Path(folder).resolve()
            self.assertTrue(root.is_relative_to(private.resolve()))
            source = root / "synthetic input"
            source.mkdir()
            (source / "sub").mkdir()
            (source / "sub" / "payload.txt").write_bytes(b"synthetic payload\n")
            iso = root / "synthetic.iso"
            tool = os.environ["EXTRACT_XISO_TEST_TOOL"]
            subprocess.run([tool, "-m", "-c", str(source), str(iso)], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            before = sha256(iso)
            output = root / "result"
            disc = extract(iso, output, tool)
            self.assertEqual((disc / "sub/payload.txt").read_bytes(), b"synthetic payload\n")
            # Exercise the same defaults as a dropped ISO, including spaces in paths.
            package = root / "drop package"
            artifacts = package / "tools/artifacts"
            artifacts.mkdir(parents=True)
            shutil.copy2(tool, artifacts / "extract-xiso.exe")
            script = package / "tools/extract_iso.py"
            shutil.copy2(Path(__file__).with_name("extract_iso.py"), script)
            subprocess.run([sys.executable, str(script), str(iso)], check=True,
                           cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual((package / "private/imported-disc/disc/sub/payload.txt").read_bytes(),
                             b"synthetic payload\n")
            self.assertEqual(before, sha256(iso))
            self.assertTrue((output / "receipt.json").exists())
            with self.assertRaises(FileExistsError):
                extract(iso, output, tool)
            self.assertEqual((disc / "sub/payload.txt").read_bytes(), b"synthetic payload\n")


if __name__ == "__main__":
    unittest.main()
