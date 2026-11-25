# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic source authentication and target-custody checks."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import shutil
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

from build_game import (STEPS, build, build_fraction, build_jobs, drop_owned_extraction, failure_excerpt,
                        available_memory, drop_stale_cache, import_disc, linux_compilers,
                        lifter_revision, main, merge_functions, precompile_shaders, program_manifest,
                        reusable_program, selected_xbe, use_windows_newlines, write_functions)
from extract_iso import sha256


class BuildTests(unittest.TestCase):
    def test_linux_build_stops_before_import_in_a_folder_the_shell_misreads(self):
        with patch("build_game.sys.platform", "linux"), patch("build_game.import_disc") as imported:
            for root in ("Downloads/DOA2U-0.1 (1).8-Alpha", "Downloads/DOA2U\nAlpha"):
                with patch("build_game.ROOT", root), self.assertRaisesRegex(ValueError, "Rename or move"):
                    build(iso=Path("disc.iso"))
            imported.assert_not_called()
            with patch("build_game.ROOT", "Downloads/DOA2U 0.1.8-Alpha"), \
                    patch("build_game.shutil.which", return_value=None), \
                    self.assertRaisesRegex(ValueError, "CMake"):
                build(iso=Path("disc.iso"))
            imported.assert_not_called()

    def test_pipless_build_stops_before_import(self):
        with patch("build_game.sys.platform", "linux"), patch("build_game.import_disc") as imported, \
                patch("build_game.ROOT", "Downloads/DOA2U"), patch("build_game.on_steamos", return_value=False), \
                patch("build_game.shutil.which", return_value="/usr/bin/tool"), \
                patch.dict("sys.modules", {"capstone": None}), \
                patch("build_game.importlib.util.find_spec", return_value=None), \
                self.assertRaisesRegex(ValueError, "has no pip"):
            build(iso=Path("disc.iso"))
        imported.assert_not_called()

    def test_steamos_build_stops_before_import_and_names_the_container_script(self):
        with patch("build_game.sys.platform", "linux"), patch("build_game.import_disc") as imported, \
                patch("build_game.ROOT", "Downloads/DOA2U"), patch("build_game.on_steamos", return_value=True), \
                self.assertRaisesRegex(ValueError, "build_steamdeck.sh"):
            build(iso=Path("disc.iso"))
        imported.assert_not_called()

    def test_jobs_fit_every_chunk_in_free_memory(self):
        gib = 1 << 30
        self.assertEqual(build_jobs(True, cpus=16, memory=24 * gib), 16)
        self.assertEqual(build_jobs(False, cpus=16, memory=24 * gib), 12)
        self.assertEqual(build_jobs(True, cpus=8, memory=6 * gib + gib // 2), 6)
        self.assertEqual(build_jobs(False, cpus=8, memory=gib), 1)
        self.assertEqual(build_jobs(True, cpus=16, memory=None), 4)

    def test_cache_naming_another_compiler_is_dropped(self):
        with tempfile.TemporaryDirectory() as folder:
            cache = Path(folder) / "CMakeCache.txt"
            for cached, compiler, kept in (("STRING=/usr/bin/clang", "/usr/bin/clang", True),
                                           ("STRING=/usr/bin/clang", "/usr/bin/gcc", False),
                                           ("FILEPATH=/usr/bin/cc", "/usr/bin/clang", False),
                                           ("FILEPATH=/usr/bin/cc", None, True)):
                cache.write_text(f"CMAKE_C_COMPILER:{cached}\nCMAKE_C_COMPILER_AR:FILEPATH=/usr/bin/ar\n")
                drop_stale_cache(Path(folder), compiler)
                self.assertEqual(cache.is_file(), kept, (cached, compiler))

    def test_free_memory_stays_within_a_container_limit(self):
        files = {"/proc/meminfo": "MemTotal: 32000000 kB\nMemAvailable: 24000000 kB\n",
                 "/sys/fs/cgroup/memory.max": "8000000000\n", "/sys/fs/cgroup/memory.current": "2000000000\n"}

        def fake_open(path, *args, **kwargs):
            if path not in files:
                raise FileNotFoundError(path)
            return io.StringIO(files[path])
        with patch("builtins.open", fake_open):
            self.assertEqual(available_memory(), 6000000000)
            files["/sys/fs/cgroup/memory.stat"] = "anon 1000000000\ninactive_file 1500000000\n"
            self.assertEqual(available_memory(), 7500000000)  # page cache the kernel can drop
            files["/sys/fs/cgroup/memory.max"] = "max\n"
            self.assertEqual(available_memory(), 24000000 * 1024)

    def test_linux_prefers_clang_unless_the_environment_names_a_compiler(self):
        tools = {"clang": "/usr/bin/clang", "clang++": "/usr/bin/clang++"}
        with patch.dict("build_game.os.environ", {}, clear=True):
            with patch("build_game.shutil.which", side_effect=tools.get):
                self.assertEqual(linux_compilers(), ("/usr/bin/clang", "/usr/bin/clang++"))
            with patch("build_game.shutil.which", side_effect={"clang": "/usr/bin/clang"}.get):
                self.assertIsNone(linux_compilers())
        with patch.dict("build_game.os.environ", {"CC": "gcc"}, clear=True), \
                patch("build_game.shutil.which", side_effect=tools.get):
            self.assertIsNone(linux_compilers())

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
        self.assertAlmostEqual(build_fraction(plan, 2, 27, 0), (5 + 1440 + 160 * 27 / 55) / total)
        # A silent, slow disassembly moves by time but stops short of its end.
        self.assertAlmostEqual(build_fraction(plan, 1, 0, 10**6), (5 + 1440 * 0.95) / total)
        self.assertAlmostEqual(build_fraction(plan, 1, 500, 0), (5 + 1440 * 0.99) / total)
        self.assertEqual(build_fraction(plan, len(plan), 0, 0), 1)
        self.assertEqual(build_fraction(["configure", "build"], 1, 0, 0), 30 / 255)
        # The compiler's up-front file list does not move the bar.
        self.assertEqual(build_fraction(["build"], 0, 109, 0), 0)
        self.assertAlmostEqual(build_fraction(["build"], 0, 0, 10**6), 0.95)

    def test_full_build_reuses_only_an_unchanged_program_from_the_same_recipe(self):
        with tempfile.TemporaryDirectory() as folder:
            generated, receipt = Path(folder) / "generated", Path(folder) / "receipt.json"
            self.assertIsNone(reusable_program(receipt, generated, {"xbe": "a"}, False))
            generated.mkdir()
            for name in ("recomp_0000.c", "recomp_dispatch.c", "recomp_funcs.h"):
                (generated / name).write_text("int x;\n")
            manifest, ebp = program_manifest(generated)
            receipt.write_text(json.dumps({"recipe": {"xbe": "a"}, "manifest_sha256": manifest, "ebp_count": ebp}))
            self.assertIsNotNone(reusable_program(receipt, generated, {"xbe": "a"}, False))
            self.assertIsNone(reusable_program(receipt, generated, {"xbe": "b"}, False))
            self.assertIsNone(reusable_program(receipt, generated, {"xbe": "a"}, True))  # not the pinned program
            with patch("build_game.PROGRAM_SHA256", manifest):
                self.assertIsNotNone(reusable_program(receipt, generated, {"xbe": "a"}, True))
            valid = receipt.read_text()
            receipt.write_text("[]")  # parses, but is no receipt
            self.assertIsNone(reusable_program(receipt, generated, {"xbe": "a"}, False))
            receipt.write_text(valid)
            (generated / "recomp_0000.c").write_text("int y;\n")
            self.assertIsNone(reusable_program(receipt, generated, {"xbe": "a"}, False))

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
                # A different ISO replaces the extraction instead of being refused outright.
                def replace(iso, output, extractor):
                    (output / "disc").mkdir(parents=True)
                    (output / "receipt.json").write_text(json.dumps(dict(iso_sha256=sha256(iso))))
                    (output / "disc/b.xbe").write_bytes(b"game")

                with patch("build_game.ROOT", root), patch("build_game.extract", side_effect=replace) as extract:
                    import_disc(iso)
                    extract.assert_called_once()
                    self.assertFalse((disc / "a.xbe").exists())
                    self.assertEqual(json.loads((root / "private/target.json").read_text())["xbe_path"],
                                     "private/imported-disc/disc/b.xbe")
                # An unsupported ISO stages, fails clean, and leaves the good import untouched.
                def bogus(iso, output, extractor):
                    (output / "disc").mkdir(parents=True)
                    (output / "receipt.json").write_text(json.dumps(dict(iso_sha256=sha256(iso))))
                    (output / "disc/c.xbe").write_bytes(b"other")

                iso.write_bytes(b"unwanted image")
                with patch("build_game.ROOT", root), patch("build_game.extract", side_effect=bogus):
                    with self.assertRaises(ValueError):
                        import_disc(iso)
                self.assertFalse((root / "private/imported-disc.staging").exists())
                self.assertEqual((disc / "b.xbe").read_bytes(), b"game")
                self.assertEqual((root / "private/imported-disc/receipt.json").read_text(),
                                 json.dumps(dict(iso_sha256=hashlib.sha256(b"another image").hexdigest())))
                self.assertEqual(selected_xbe(), (disc / "b.xbe").resolve())
                iso.write_bytes(b"another image")
            with patch("build_game.ROOT", root), patch("build_game.XBE_SHA256", "0" * 64):
                with self.assertRaises(ValueError):
                    import_disc(iso)

    def test_built_extraction_is_dropped_and_reused_from_the_play_folder(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            imported, play = root / "private/imported-disc", root / "private/play-disc"
            iso = root / "synthetic.iso"
            iso.write_bytes(b"synthetic image")

            def fresh(iso, output, extractor):
                (output / "disc/sub").mkdir(parents=True)
                (output / "receipt.json").write_text(json.dumps(dict(iso_sha256=sha256(iso))))
                (output / "disc/b.xbe").write_bytes(b"game")
                (output / "disc/sub/data.afs").write_bytes(b"data")

            with patch("build_game.ROOT", root), patch("build_game.XBE_SHA256", hashlib.sha256(b"game").hexdigest()):
                with patch("build_game.extract", side_effect=fresh):
                    import_disc(iso)
                # Saves in the extraction keep it whole.
                (imported / "disc/.recomp-storage").mkdir()
                drop_owned_extraction()
                self.assertTrue((imported / "disc/b.xbe").is_file())
                self.assertFalse(play.exists())
                (imported / "disc/.recomp-storage").rmdir()
                # A play folder missing a file is never left as the only copy.
                (play / "sub").mkdir(parents=True)
                (play / "b.xbe").write_bytes(b"game")
                drop_owned_extraction()
                self.assertTrue((imported / "disc/sub/data.afs").is_file())
                # Nor is one from another import with the same names but other bytes.
                (play / "sub/data.afs").write_bytes(b"old!")
                drop_owned_extraction()
                self.assertTrue((imported / "disc/sub/data.afs").is_file())
                shutil.rmtree(play)
                # A copied play folder with the same bytes counts as whole.
                shutil.copytree(imported / "disc", play)
                # A held file keeps the extraction and does not fail the build.
                with patch("build_game.Path.rename", side_effect=PermissionError("held")):
                    drop_owned_extraction()
                self.assertTrue((imported / "disc/b.xbe").is_file())
                self.assertFalse((imported / "disc.old").exists())

                drop_owned_extraction()
                self.assertFalse((imported / "disc").exists())
                self.assertFalse((imported / "disc.old").exists())
                self.assertTrue((imported / "receipt.json").is_file())
                self.assertEqual((play / "sub/data.afs").read_bytes(), b"data")
                self.assertEqual(selected_xbe(), (play / "b.xbe").resolve())
                with patch("build_game.extract") as extract:
                    import_disc(iso)
                extract.assert_not_called()
                self.assertEqual(selected_xbe(), (play / "b.xbe").resolve())
                # Without the play folder the ISO is extracted again.
                shutil.rmtree(play)
                with patch("build_game.extract", side_effect=fresh) as extract:
                    import_disc(iso)
                extract.assert_called_once()
                self.assertEqual(selected_xbe(), (imported / "disc/b.xbe").resolve())

    def test_packaged_lifter_uses_the_export_pin(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "lifter").mkdir()
            links = [dict(path="tools/xboxrecomp", commit="a" * 40)]
            (root / "public-export.json").write_text(json.dumps(dict(gitlinks=links)))
            with patch("build_game.ROOT", root), patch("build_game.LIFTER", root / "lifter"):
                self.assertEqual(lifter_revision(), "a" * 40)


class PrecompileTests(unittest.TestCase):
    """The precompile-only runner pass a build runs, so the next launch finds its shaders."""

    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)
        (self.root / "private").mkdir()
        self.logs = self.root / "private/build-logs/abc"
        self.logs.mkdir(parents=True)
        self.xbe = self.root / "private/game.xbe"
        self.xbe.write_bytes(b"synthetic game")
        self.runners = {False: self.root / "build/runner.exe", True: self.root / "build/runner_vulkan.exe"}

    def run_precompile(self, code=0):
        def runner(root=None, vulkan=False):
            return self.runners[vulkan]

        output = io.StringIO()
        with patch("build_game.ROOT", self.root), patch("build_game.sys.platform", "win32"), \
                patch("build_game.selected_xbe", return_value=self.xbe), \
                patch("build_game.runner_path", runner), \
                patch("build_game.subprocess.call", return_value=code) as call, \
                contextlib.redirect_stdout(output):
            precompile_shaders(self.logs)
        return call, output.getvalue()

    def test_every_runner_fills_the_cache_a_launch_reads(self):
        call, output = self.run_precompile()
        self.assertEqual(len(call.call_args_list), 2)
        for called, runner in zip(call.call_args_list, self.runners.values()):
            self.assertEqual(called.args[0][0], str(runner))
            self.assertEqual(called.args[0][1:], ["--xbe", str(self.xbe), "--precompile-shaders"])
            self.assertEqual(called.kwargs["env"]["RECOMP_SHADER_CACHE"],
                             str(self.root / "private/shader-cache"))
        self.assertEqual(sorted(path.name for path in self.logs.iterdir()),
                         ["precompile-runner.log", "precompile-runner_vulkan.log"])
        self.assertIn("Precompiled draw shaders for runner.exe", output)

    def test_a_failed_runner_only_warns_and_the_next_one_still_runs(self):
        call, output = self.run_precompile(code=5)
        self.assertEqual(len(call.call_args_list), 2)
        self.assertIn("Warning", output)
        self.assertIn("exit code 5", output)
        self.assertIn("next launch", output)

    def test_lift_only_skips_the_precompile(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "private/program/generated").mkdir(parents=True)
            (root / "private/program/functions.json").write_text("[]")
            runner = root / "build/recomp-program/recomp_program_runner"
            runner.parent.mkdir(parents=True)
            runner.write_bytes(b"runner")
            xbe = root / "private/game.xbe"
            xbe.write_bytes(b"game")
            capstone = types.SimpleNamespace(__version__="5.0.7")
            with patch("build_game.ROOT", root), patch("build_game.sys.platform", "darwin"), \
                    patch("build_game.Progress"), patch("build_game.selected_xbe", return_value=xbe), \
                    patch("build_game.runner_path", return_value=runner), \
                    patch("build_game.lifter_revision", return_value="a" * 40), \
                    patch("build_game.merge_functions"), \
                    patch("build_game.program_manifest", return_value=("0" * 64, 0)), \
                    patch.dict("sys.modules", {"capstone": capstone}), \
                    patch("build_game.precompile_shaders") as precompile, \
                    contextlib.redirect_stdout(io.StringIO()):
                build(lift_only=True)
                precompile.assert_not_called()
                build()
                precompile.assert_called_once()


class SoundtrackTests(unittest.TestCase):
    """The optional Jukebox steps after a build; they read the play folder a build leaves."""

    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)
        (self.root / "private/play-disc/DOA2").mkdir(parents=True)
        (self.root / "private/play-disc/DOA2/bgm.afs").write_bytes(b"synthetic music")
        (self.root / "jukebox").mkdir()

    def run_main(self, argv, call):
        with patch("build_game.ROOT", self.root), patch("build_game.build"), \
                patch("build_game.sys.argv", ["build_game.py", *argv]), \
                patch("build_game.subprocess.call", call):
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                result = main()
        return result, output.getvalue()

    def test_lift_only_skips_the_soundtrack_steps_it_cannot_finish(self):
        call = Mock(return_value=0)
        self.assertEqual(self.run_main(["--lift-only"], call), (0, ""))
        call.assert_not_called()

    def test_import_reads_the_play_folder_the_build_leaves(self):
        call = Mock(return_value=0)
        result, output = self.run_main([], call)
        self.assertEqual((result, output), (0, ""))
        commands = [called.args[0] for called in call.call_args_list]
        self.assertEqual(commands[0][1:], [str(self.root / "tools/import_soundtrack.py"),
                                           str(self.root / "private/play-disc")])
        self.assertEqual(commands[1][1:], [str(self.root / "tools/import_soundtrack.py"), "--level"])

    def test_failed_loudness_level_warns_without_failing_the_build(self):
        for album in ("doa1u", "doa2u-movies"):
            (self.root / "jukebox" / album).mkdir()
            (self.root / "jukebox" / album / "tracks.txt").write_text("#title\tsynthetic\n")
        call = Mock(return_value=3)
        result, output = self.run_main([], call)
        self.assertEqual(result, 0)
        self.assertEqual(len(call.call_args_list), 1)
        self.assertEqual(call.call_args.args[0][-1], "--level")
        self.assertIn("exited with 3", output)
        self.assertIn("loudness", output)

    def test_failed_import_warns_without_failing_the_build(self):
        call = Mock(return_value=1)
        result, output = self.run_main([], call)
        self.assertEqual(result, 0)
        self.assertIn("exited with 1", output)
        self.assertIn("movies' music is missing", output)


if __name__ == "__main__":
    unittest.main()
