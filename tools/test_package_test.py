# SPDX-License-Identifier: GPL-3.0-or-later
import unittest
from pathlib import Path
from unittest import mock

import package_test
from package_test import PLATFORM_FILES, git, lifter_shipped, shipped


class ReleaseSelectionTest(unittest.TestCase):
    def test_windows_release_needs_the_launcher_icon(self):
        # Without it the ZIP's exes lose their icon, and updating replaces the installed ones.
        with mock.patch.object(package_test, "ICON", Path("missing.ico")), \
                mock.patch("sys.argv", ["package_test.py", "0.0-test"]):
            with self.assertRaisesRegex(SystemExit, "launcher"):
                package_test.main()

    def test_platform_lists_name_tracked_files(self):
        # A renamed backend would otherwise ship to every platform.
        tracked = set(git("ls-files").decode().splitlines())
        for platform, files in PLATFORM_FILES.items():
            self.assertEqual(files - tracked, set(), platform)

    def test_each_platform_ships_its_own_backends(self):
        self.assertTrue(shipped("recomp-runtime/d3d_presenter_d3d11.cpp", "windows"))
        self.assertFalse(shipped("recomp-runtime/d3d_presenter_d3d11.cpp", "linux"))
        self.assertFalse(shipped("recomp-runtime/overlay_d3d11.cpp", "macos"))
        # Windows builds the SDL_GPU presenter as its Vulkan renderer.
        for name in ("d3d_presenter_sdl_gpu.cpp", "d3d_presenter_inline.cpp", "d3d_shader_compiler.cpp",
                     "d3d_shader_compiler.h", "overlay_sdl_gpu.cpp"):
            self.assertTrue(shipped(f"recomp-runtime/{name}", "windows"), name)
        self.assertFalse(shipped("build_game.sh", "windows"))
        self.assertTrue(shipped("docs/building.md", "macos"))
        self.assertFalse(shipped("tools/launcher_sdl.cpp", "macos"))
        self.assertTrue(shipped("recomp-runtime/kernel_file.c", "linux"))
        # build_game.py --disassemble reads it.
        self.assertTrue(shipped("tools/doa2u/seed_functions.json", "windows"))

    def test_developer_notes_stay_out(self):
        for path in ("AGENTS.md", "CONTEXT.md", "README.md", "docs/bring-up.md", "tools/test_run_game.py"):
            self.assertFalse(shipped(path, "windows"), path)
        self.assertTrue(lifter_shipped("tools/recomp/__main__.py"))
        self.assertTrue(lifter_shipped("LICENSES/LGPL-2.1.txt"))
        self.assertTrue(lifter_shipped("templates/runtime/recomp_types.h"))
        self.assertFalse(lifter_shipped("tools/recomp/test_lifter_fpu.py"))
        self.assertFalse(lifter_shipped("README.md"))
        self.assertFalse(lifter_shipped("src/kernel/kernel.c"))


if __name__ == "__main__":
    unittest.main()
