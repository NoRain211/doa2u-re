# SPDX-License-Identifier: GPL-3.0-or-later
"""What tools/xbe_info.py prints and reads, on a synthetic Xbox executable."""
import contextlib
import hashlib
import io
import os
import struct
import sys
import tempfile
import unittest
from unittest import mock

import xbe_info


BASE = 0x10000
SECTION_VA = 0x100000
SECTION_VIRTUAL = 0x1280
SECTION_ON_DISC = 0x100
LIBRARY_BUILD = 5849
LIBRARY_FLAGS = 0x8001


def xbe_image():
    """A tiny XBE: one .bss-like section and one D3D8 library version entry."""
    data = bytearray(0x500)
    data[:4] = b"XBEH"
    struct.pack_into("<I", data, 0x104, BASE)
    struct.pack_into("<II", data, 0x11C, 1, BASE + 0x200)
    struct.pack_into("<II", data, 0x160, 1, BASE + 0x480)
    struct.pack_into("<IIIIII", data, 0x200, 0, SECTION_VA, SECTION_VIRTUAL, 0x300,
                     SECTION_ON_DISC, BASE + 0x4C0)
    data[0x4C0:0x4C6] = b".bss\0\0"
    struct.pack_into("<8sHHHH", data, 0x480, b"D3D8\0\0\0\0", 1, 0, LIBRARY_BUILD, LIBRARY_FLAGS)
    data[0x300:0x300 + SECTION_ON_DISC] = b"\x90" * SECTION_ON_DISC
    return bytes(data)


class XbeInfoTests(unittest.TestCase):
    def test_main_prints_the_library_build_and_the_virtual_size(self):
        data = xbe_image()
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "DOA2.xbe")
            with open(path, "wb") as file:
                file.write(data)
            printed = io.StringIO()
            with mock.patch.object(sys, "argv", ["xbe_info.py", path]), \
                    contextlib.redirect_stdout(printed):
                xbe_info.main()
        self.assertEqual(printed.getvalue().splitlines(), [
            f"D3D8 {LIBRARY_BUILD}",
            f"section .bss {hex(SECTION_VA)} {SECTION_VIRTUAL}",
            f"sha256 {hashlib.sha256(data).hexdigest()}",
        ])

    def test_reads_stop_at_the_bytes_on_the_disc(self):
        data = xbe_image()
        _base, sections = xbe_info.load(data)
        self.assertEqual(sections[0], (SECTION_VA, SECTION_VIRTUAL, SECTION_ON_DISC, 0x300, ".bss"))
        self.assertEqual(xbe_info.read(data, sections, SECTION_VA, SECTION_ON_DISC),
                         b"\x90" * SECTION_ON_DISC)
        with self.assertRaises(ValueError):
            xbe_info.read(data, sections, SECTION_VA, SECTION_ON_DISC + 1)
        self.assertEqual(xbe_info.at(sections, SECTION_VA), (0x300, SECTION_ON_DISC))
        self.assertIsNone(xbe_info.at(sections, SECTION_VA + SECTION_ON_DISC))


if __name__ == "__main__":
    unittest.main()
