# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic checks for hash-pinned binary files in the public export."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from public_export import ExportError, load_manifest, verify_entries

DATA = b"\x00synthetic binary"


def manifest(pins, include=("art/**", "private/**")):
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "public-export.json"
        path.write_text(json.dumps({
            "version": 1, "include": list(include), "quarantine": [], "allowed_private_paths": [],
            "forbidden_extensions": [".xbe"], "byte_literal_exceptions": [], "max_file_bytes": 64,
            "binary_files": [{"path": p, "sha256": hashlib.sha256(DATA).hexdigest()} for p in pins]}))
        return load_manifest(path)


class BinaryFileTests(unittest.TestCase):
    def test_pinned_binary_passes_only_with_its_hash(self):
        self.assertEqual(verify_entries({"art/a.jpg": DATA}, manifest(["art/a.jpg"]), True), (1, 0))
        with self.assertRaisesRegex(ExportError, "reviewed sha256"):
            verify_entries({"art/a.jpg": DATA + b"x"}, manifest(["art/a.jpg"]), True)

    def test_pinned_binary_keeps_path_and_extension_rules(self):
        for path, error in (("art/game.xbe", "forbidden extension"), ("private/a.jpg", "not allowlisted")):
            with self.assertRaisesRegex(ExportError, error):
                verify_entries({path: DATA}, manifest([path]), True)

    def test_duplicate_pins_are_rejected(self):
        with self.assertRaisesRegex(ExportError, "duplicate"):
            manifest(["art/a.jpg", "art/a.jpg"])


if __name__ == "__main__":
    unittest.main()
