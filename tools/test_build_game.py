# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic source authentication and target-custody checks."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from build_game import (STEPS, build_fraction, failure_excerpt, import_disc, lifter_revision,
                        merge_functions, program_manifest, selected_xbe, use_windows_newlines,
                        write_functions)
from extract_iso import sha256


class BuildTests(unittest.TestCase):
    def test_failure_excerpt_keeps_an_early_cause_ahead_of_a_long_tail(self):
        # CMake prints "Could not create named generator" first, then its whole help.
        help_text = [f"  Generator {i}" for i in range(70)]
        excerpt = failure_excerpt(["CMake Error: Could not create named generator X", "", *help_text])
        self.assertEqual(excerpt[0], "CMake Error: Could not create named generator X")
        self.assertEqual(excerpt[20:22], ["...", help_text[-20]])
        self.assertEqual(excerpt[-1], help_text[-1])
        # An error inside the tail, or none, leaves the plain tail; short logs print whole.
        tail = [f"line {i}" for i in range(30)]
        self.assertEqual(failure_excerpt([*tail, "error: last"]), [*tail[11:], "error: last"])
        self.assertEqual(failure_excerpt(tail), tail[-20:])
        # An early error close to the tail joins it without a gap.
        self.assertEqual(failure_excerpt(["error: near", *tail[:20]]), ["error: near", *tail[:20]])
        self.assertEqual(failure_excerpt(["error: only"]), ["error: only"])

    def test_function_shards_round_trip_without_call_edges(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            functions = [{"start": hex(i), "calls_to": [], "called_by": ["0x1"]} for i in range(2500)]
            (root / "functions.json").write_text(json.dumps(functions))
            (root / "shards").mkdir()
            (root / "shards/99.json").write_text("[]")
            with patch("build_game.FUNCTIONS", root / "shards"):
                write_functions(root)
                merge_functions(root / "merged.json")
            self.assertEqual(sorted(p.name for p in (root / "shards").iterdir()), ["00.json", "01.json", "02.json"])
            self.assertEqual(json.loads((root / "merged.json").read_text()), [{"start": hex(i)} for i in range(2500)])

    def test_progress_counts_finished_steps_then_output_or_time(self):
        plan = list(STEPS)
        total = sum(seconds for seconds, _ in STEPS.values())
        self.assertEqual(build_fraction(plan, 0, 0, 0), 0)
        # Half the lift's output after the parser and disassembly.
        self.assertAlmostEqual(build_fraction(plan, 2, 27, 0), (5 + 1440 + 315 * 27 / 55) / total)
        # A silent, slow disassembly moves by time but stops short of its end.
        self.assertAlmostEqual(build_fraction(plan, 1, 0, 10**6), (5 + 1440 * 0.95) / total)
        self.assertAlmostEqual(build_fraction(plan, 1, 500, 0), (5 + 1440 * 0.99) / total)
        self.assertEqual(build_fraction(plan, len(plan), 0, 0), 1)
        self.assertEqual(build_fraction(["configure", "build"], 1, 0, 0), 30 / 275)
        # The compiler's up-front file list does not move the bar.
        self.assertEqual(build_fraction(["build"], 0, 109, 0), 0)
        self.assertAlmostEqual(build_fraction(["build"], 0, 0, 10**6), 0.95)

    def test_manifest_authenticates_members_and_contents(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "recomp_0000.c").write_text("void synthetic(void) {\n    uint32_t ebp;\n}\n")
            (root / "recomp_dispatch.c").write_text("/* synthetic dispatcher */\n")
            (root / "recomp_funcs.h").write_text("void synthetic(void);\n")
            before, count = program_manifest(root)
            self.assertEqual(count, 1)
            (root / "recomp_funcs.h").write_text("void changed(void);\n")
            self.assertNotEqual(before, program_manifest(root)[0])
            (root / "recomp_0002.c").write_text("/* gap */\n")
            with self.assertRaises(ValueError):
                program_manifest(root)

    def test_generated_files_get_the_windows_line_endings_the_manifest_pins(self):
        with tempfile.TemporaryDirectory() as folder:
            generated = Path(folder)
            (generated / "recomp_0000.c").write_bytes(b"a\nb\r\nc\n")
            with patch("build_game.sys.platform", "darwin"):
                use_windows_newlines(generated)
            self.assertEqual((generated / "recomp_0000.c").read_bytes(), b"a\r\nb\r\nc\r\n")
            (generated / "recomp_0000.c").write_bytes(b"a\n")
            with patch("build_game.sys.platform", "win32"):
                use_windows_newlines(generated)
            self.assertEqual((generated / "recomp_0000.c").read_bytes(), b"a\n")

    def test_target_hash_and_private_boundary(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "private").mkdir()
            target = root / "private/target.json"
            xbe = root / "private/synthetic.input"
            xbe.write_bytes(b"synthetic input")
            identity = dict(xbe_path="private/synthetic.input", xbe_sha256=sha256(xbe))
            target.write_text(json.dumps(identity))
            with patch("build_game.ROOT", root):
                self.assertEqual(selected_xbe(), xbe.resolve())
                xbe.write_bytes(b"changed")
                with self.assertRaises(ValueError):
                    selected_xbe()
                outside = root / "outside.input"
                outside.write_bytes(b"outside")
                identity.update(xbe_path="private/../outside.input", xbe_sha256=sha256(outside))
                target.write_text(json.dumps(identity))
                with self.assertRaises(ValueError):
                    selected_xbe()

    def test_import_reuses_matching_extraction_and_selects_by_hash(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            disc = root / "private/imported-disc/disc"
            disc.mkdir(parents=True)
            iso = root / "synthetic.iso"
            iso.write_bytes(b"synthetic image")
            (root / "private/imported-disc/receipt.json").write_text(json.dumps(dict(iso_sha256=sha256(iso))))
            (disc / "a.xbe").write_bytes(b"launcher")
            (disc / "b.xbe").write_bytes(b"game")
            (root / "private/target.json").write_text(json.dumps(dict(note="kept")))
            with patch("build_game.ROOT", root), patch("build_game.XBE_SHA256", sha256(disc / "b.xbe")), \
                    patch("build_game.extract") as extract:
                import_disc(iso)
                extract.assert_not_called()
                target = json.loads((root / "private/target.json").read_text())
                self.assertEqual(target["xbe_path"], "private/imported-disc/disc/b.xbe")
                self.assertEqual(target["note"], "kept")
                self.assertEqual(selected_xbe(), (disc / "b.xbe").resolve())
                iso.write_bytes(b"another image")
                with self.assertRaises(ValueError):
                    import_disc(iso)
            with patch("build_game.ROOT", root), patch("build_game.XBE_SHA256", "0" * 64):
                iso.write_bytes(b"synthetic image")
                with self.assertRaises(ValueError):
                    import_disc(iso)

    def test_packaged_lifter_uses_the_export_pin(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "lifter").mkdir()
            links = [dict(path="tools/xboxrecomp", commit="a" * 40)]
            (root / "public-export.json").write_text(json.dumps(dict(gitlinks=links)))
            with patch("build_game.ROOT", root), patch("build_game.LIFTER", root / "lifter"):
                self.assertEqual(lifter_revision(), "a" * 40)


if __name__ == "__main__":
    unittest.main()
