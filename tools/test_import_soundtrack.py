# SPDX-License-Identifier: GPL-3.0-or-later
import os
import struct
import tempfile
import unittest

import import_soundtrack as soundtrack


def xdvdfs_image(path, files, base=0):
    """A tiny Xbox disc image: a root directory holding files {name: bytes} and SOUND/<name> for paths with '/'."""
    sector = soundtrack.SECTOR
    image = bytearray(base + 40 * sector)
    data_at = 36

    def directory(entries):
        """entries: [(name, sector, size, is_dir)] as a right-leaning tree."""
        table = bytearray()
        for i, (name, start, size, is_dir) in enumerate(entries):
            raw = name.encode()
            length = 14 + len(raw)
            length += (-length) % 4
            right = (len(table) + length) // 4 if i + 1 < len(entries) else 0
            table += struct.pack("<HHIIBB", 0, right, start, size, 0x10 if is_dir else 0x80, len(raw)) + raw
            table += b"\xff" * (length - 14 - len(raw))
        return bytes(table)

    root, sub = [], []
    for name, content in files.items():
        start = data_at
        data_at += (len(content) + sector - 1) // sector
        image[base + start * sector:base + start * sector + len(content)] = content
        if "/" in name:
            sub.append((name.split("/")[1], start, len(content), False))
        else:
            root.append((name, start, len(content), False))
    if sub:
        table = directory(sub)
        image[base + 34 * sector:base + 34 * sector + len(table)] = table
        root.append(("SOUND", 34, len(table), True))
    table = directory(root)
    image[base + 33 * sector:base + 33 * sector + len(table)] = table
    image[base + 32 * sector:base + 32 * sector + 28] = soundtrack.MAGIC + struct.pack("<II", 33, len(table))
    with open(path, "wb") as file:
        file.write(image)


def afs(entries):
    head = b"AFS\0" + struct.pack("<I", len(entries))
    offset, body, table = 2048, b"", b""
    for entry in entries:
        table += struct.pack("<II", offset, len(entry))
        body += entry + bytes((-len(entry)) % 2048)
        offset += len(entry) + (-len(entry)) % 2048
    head += table
    return head + bytes(2048 - len(head)) + body


class NameTests(unittest.TestCase):
    def test_doa4_names(self):
        self.assertEqual(soundtrack.doa4_name("bgm_aya"), "Ayane")
        self.assertEqual(soundtrack.doa4_name("rmx_kas"), "Kasumi (Remix)")
        self.assertEqual(soundtrack.doa4_name("bgm_hel_doa2"), "Helena (DOA2)")
        self.assertEqual(soundtrack.doa4_name("bgm_select"), "Character Select")
        self.assertEqual(soundtrack.doa4_name("bgm_spa"), "Spartan-458")

    def test_group_names(self):
        self.assertEqual(soundtrack.group_name(["rmx_kas", "rmx_alp"]), "Kasumi / Alpha-152 (Remix)")
        self.assertEqual(soundtrack.group_name(["bgm_bas", "bgm_bas_doa2", "bgm_bas_doa3"]), "Bass")
        self.assertEqual(soundtrack.group_name(["bgm_select", "bgm_ups"]), "Character Select")


class DiscTests(unittest.TestCase):
    def test_reads_and_detects_discs(self):
        adx = b"\x80\x00\x00\x20" + bytes(100)
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "doa3.iso")
            xdvdfs_image(path, {"bgm.afs": afs([adx, adx + b"x", b"\x77" * 16]), "voice.afs": b"AFS\0"})
            disc = soundtrack.Xdvdfs(path)
            try:
                self.assertEqual(soundtrack.detect(disc), "doa3")
                self.assertEqual(disc.read("BGM.AFS")[:3], b"AFS")
                self.assertIsNone(disc.find("SOUND/bgm_stream.xwb"))
                out = os.path.join(folder, "out")
                self.assertEqual(soundtrack.import_doa3(disc, out), 2)  # the 16-byte padding entry is left out
            finally:
                disc.close()
            with open(os.path.join(out, "doa3", "tracks.txt"), encoding="utf-8") as file:
                lines = file.read().splitlines()
            self.assertEqual(lines[0], "#title\tDead or Alive 3")
            self.assertEqual(lines[2], "01.adx\t-1\t-1\tKasumi")
            # Renamed tracks keep their names when the disc is imported again.
            with open(os.path.join(out, "doa3", "tracks.txt"), "w", encoding="utf-8") as file:
                file.write("\n".join(lines[:2] + ["01.adx\t-1\t-1\tMy Name"] + lines[3:]) + "\n")
            self.assertEqual(soundtrack.read_names(os.path.join(out, "doa3"))["01.adx"], "My Name")

    def test_sfd_audio(self):
        adx = b"\x80\x00\x00\x20" + bytes(100)
        pes = lambda sid, payload, head=b"\x0f": b"\0\0\1" + bytes([sid]) + struct.pack(">H", len(head) + len(payload)) + head + payload
        pack = b"\0\0\1\xba\x21" + bytes(7)  # MPEG-1 pack header
        movie = pack + pes(0xE0, b"video") + pes(0xC0, adx[:50], b"\xff\x21" + bytes(4)) + pack + pes(0xC0, adx[50:]) + b"\0\0\1\xb9"
        self.assertEqual(soundtrack.sfd_audio(movie), adx)
        self.assertIsNone(soundtrack.sfd_audio(pack + pes(0xE0, b"video")))

    def test_doa4_and_folders(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "doa4.iso")
            xdvdfs_image(path, {"snd.afs": b"AFS\0", "SOUND/bgm_stream.xwb": b"DNBW"})
            disc = soundtrack.Xdvdfs(path)
            try:
                self.assertEqual(soundtrack.detect(disc), "doa4")
            finally:
                disc.close()
            os.makedirs(os.path.join(folder, "x", "DOA2"))
            open(os.path.join(folder, "x", "DOA2", "bgm.afs"), "wb").close()
            self.assertEqual(soundtrack.detect(soundtrack.Folder(os.path.join(folder, "x"))), "doa2u")
            with self.assertRaises(ValueError):
                soundtrack.Xdvdfs(os.path.join(folder, "x", "DOA2", "bgm.afs"))


if __name__ == "__main__":
    unittest.main()
