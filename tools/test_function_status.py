# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks the decompilation registry against the committed lifter list."""
import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "tools/doa2u/function_status.json"
FUNCTIONS = ROOT / "tools/doa2u/functions"
STATUSES = {"named", "decompiled", "bound", "tested"}


def load_registry():
    return json.loads(REGISTRY.read_text(encoding="utf-8"))


class FunctionStatusTests(unittest.TestCase):
    def test_addresses_are_known_and_unique(self):
        functions = {
            int(entry["start"], 16)
            for shard in FUNCTIONS.glob("*.json")
            for entry in json.loads(shard.read_text(encoding="utf-8"))
        }
        seen = set()
        for entry in load_registry():
            address = int(entry["start"], 16)
            self.assertIn(address, functions, entry)
            self.assertNotIn(address, seen, entry)
            seen.add(address)
            self.assertTrue(entry["name"].strip(), entry)
            self.assertIn(entry.get("status", "named"), STATUSES, entry)

    def test_bound_entries_name_an_existing_source_symbol(self):
        for entry in load_registry():
            if entry.get("status") not in {"bound", "tested"}:
                continue
            self.assertIn("source", entry)
            self.assertIn("symbol", entry)
            source = ROOT / entry["source"]
            self.assertTrue(source.is_file(), entry)
            self.assertRegex(
                source.read_text(encoding="utf-8"),
                rf"(?m)^[ \t]*(?:[A-Za-z_][A-Za-z0-9_]*[ \t*]+)+"
                rf"{re.escape(entry['symbol'])}\s*\([^;]*\)\s*\{{",
                entry,
            )


if __name__ == "__main__":
    unittest.main()
