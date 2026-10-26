# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks the Ghidra exporter's merge rules without Ghidra."""
import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools/doa2u/ghidra/ExportFunctionStatus.py"


class FakeAddress:
    def __init__(self, offset):
        self.offset = offset

    def getOffset(self):
        return self.offset


class FakeTag:
    def __init__(self, name):
        self.name = name

    def getName(self):
        return self.name


class FakeFunction:
    def __init__(self, address, name, tag=None, comment=None):
        self.address = address
        self.name = name
        self.tag = tag
        self.comment = comment

    def getEntryPoint(self):
        return FakeAddress(self.address)

    def getName(self):
        return self.name

    def getTags(self):
        return [FakeTag(self.tag)] if self.tag else []

    def getComment(self):
        return self.comment


class FakeProgram:
    def __init__(self, path, functions):
        self.path = path
        self.functions = functions

    def getExecutablePath(self):
        return str(self.path)

    def getFunctionManager(self):
        return self

    def getFunctions(self, forward):
        return list(self.functions)


def run_export(root, registry, starts, functions, registry_path=None):
    folder = root / "tools/doa2u"
    (folder / "functions").mkdir(parents=True)
    registry_path = registry_path or folder / "function_status.json"
    registry_path.parent.mkdir(parents=True, exist_ok=True)
    registry_path.write_text(json.dumps(registry), encoding="utf-8")
    (root / "README.md").write_text(
        "Before\n\n<!-- function-status-grid:start -->\nold\n"
        "<!-- function-status-grid:end -->\n\nAfter\n",
        encoding="utf-8",
    )
    (folder / "functions/00.json").write_text(
        json.dumps([{"start": "0x%08X" % address} for address in starts]),
        encoding="utf-8",
    )
    namespace = {
        "__name__": "export_test",
        "__file__": str(SCRIPT),
        "currentProgram": FakeProgram(root / "DOA2.xbe", functions),
        "getScriptArgs": lambda: [str(registry_path)],
    }
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        exec(compile(SCRIPT.read_text(encoding="utf-8"), str(SCRIPT), "exec"), namespace)
    return json.loads(registry_path.read_text(encoding="utf-8")), output.getvalue()


class ExportFunctionStatusTests(unittest.TestCase):
    def test_refresh_drops_skipped_and_outside_names(self):
        with tempfile.TemporaryDirectory() as folder:
            entries, output = run_export(
                Path(folder),
                [
                    {"start": "0x00001000", "name": "old_name"},
                    {"start": "0x00001004", "name": "outside"},
                    {
                        "start": "0x00001008",
                        "name": "manual",
                        "status": "tested",
                        "source": "recomp-runtime/program_manual.c",
                        "symbol": "manual",
                    },
                    {
                        "start": "0x0000100C",
                        "name": "manual_skipped",
                        "status": "tested",
                        "source": "recomp-runtime/program_manual.c",
                        "symbol": "manual_skipped",
                    },
                ],
                {0x1000, 0x1008, 0x100C},
                [
                    FakeFunction(0x1000, "FUN_00001000"),
                    FakeFunction(0x1004, "outside"),
                    FakeFunction(0x100C, "FUN_0000100C"),
                ],
            )
        self.assertEqual(entries, [
            {
                "start": "0x00001008",
                "name": "manual",
                "status": "tested",
                "source": "recomp-runtime/program_manual.c",
                "symbol": "manual",
            },
            {
                "start": "0x0000100C",
                "name": "manual_skipped",
                "status": "tested",
                "source": "recomp-runtime/program_manual.c",
                "symbol": "manual_skipped",
            },
        ])
        self.assertIn("skipped 1 names outside", output)
        self.assertIn("kept 1 entries not defined in Ghidra", output)

    def test_refresh_preserves_bindings_and_marks_comments(self):
        with tempfile.TemporaryDirectory() as folder:
            entries, _ = run_export(
                Path(folder),
                [
                    {
                        "start": "0x00001000",
                        "name": "bound_name",
                        "status": "bound",
                        "source": "recomp-runtime/program_manual.c",
                        "symbol": "bound_name",
                    },
                    {"start": "0x0000100C", "name": "old_name"},
                ],
                {0x1000, 0x1004, 0x1008, 0x100C},
                [
                    FakeFunction(0x1000, "bound_name", tag="GAME", comment="known"),
                    FakeFunction(0x1004, "new_name", tag="CRI_MIDDLEWARE", comment="new"),
                    FakeFunction(0x1008, "plain_name"),
                    FakeFunction(0x100C, "renamed"),
                ],
            )
        by_start = {entry["start"]: entry for entry in entries}
        self.assertEqual(by_start["0x00001000"]["status"], "bound")
        self.assertEqual(by_start["0x00001000"]["source"], "recomp-runtime/program_manual.c")
        self.assertEqual(by_start["0x00001000"]["symbol"], "bound_name")
        self.assertEqual(by_start["0x00001000"]["tag"], "GAME")
        self.assertEqual(by_start["0x00001004"]["name"], "new_name")
        self.assertEqual(by_start["0x00001004"]["status"], "decompiled")
        self.assertEqual(by_start["0x00001004"]["tag"], "CRI_MIDDLEWARE")
        self.assertNotIn("status", by_start["0x00001008"])
        self.assertEqual(by_start["0x0000100C"]["name"], "renamed")

    def test_registry_copy_uses_repository_shards(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            output = root / "output/function_status.json"
            entries, _ = run_export(
                root,
                [{"start": "0x00001000", "name": "old_name"}],
                {0x1000},
                [FakeFunction(0x1000, "new_name")],
                registry_path=output,
            )
            self.assertTrue(output.is_file())
        self.assertEqual(entries, [{"start": "0x00001000", "name": "new_name"}])

    def test_refresh_updates_readme_grid(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            run_export(
                root,
                [{
                    "start": "0x00001004",
                    "name": "tested_name",
                    "status": "tested",
                    "source": "recomp-runtime/program_manual.c",
                    "symbol": "tested_name",
                }],
                {0x1000, 0x1004},
                [
                    FakeFunction(0x1000, "game_named", tag="GAME"),
                    FakeFunction(0x1004, "game_tested", tag="GAME"),
                ],
            )
            readme = (root / "README.md").read_text(encoding="utf-8")
        self.assertIn("| Game | 1 | 0 | 1 | 2 | 50% |", readme)
        self.assertIn("| **Total** | **1** | **0** | **1** | **2** | **50%** |", readme)
        self.assertIn("Before\n", readme)
        self.assertIn("\nAfter\n", readme)


if __name__ == "__main__":
    unittest.main()
