# SPDX-License-Identifier: GPL-3.0-or-later
import os
import struct
import tempfile
import unittest
from unittest import mock
from pathlib import Path
from types import SimpleNamespace

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


class ImportFailureTests(unittest.TestCase):
    def test_cue_and_labels_cannot_inject_paths_or_records(self):
        self.assertEqual(soundtrack.safe_cue("x/../../bad\n\t"), "x_______bad__")
        self.assertLessEqual(len(soundtrack.safe_cue("x" * 200)), 48)
        with tempfile.TemporaryDirectory() as folder:
            soundtrack.write_tracks(folder, "Album\nInjected", [("01.adx", -1, -1, "Name\tOther\nRow")])
            lines = Path(folder, "tracks.txt").read_text().splitlines()
            self.assertEqual(len(lines), 3)
            self.assertEqual(len(lines[2].split("\t")), 4)
            with self.assertRaises(ValueError):
                soundtrack.write_tracks(folder, "Album", [("x/../bad.adx", -1, -1, "Name")])

    def test_failed_level_is_retried_and_no_completion_mark_is_written(self):
        with tempfile.TemporaryDirectory() as folder:
            soundtrack.write_tracks(folder, "Album", [("one.adx", -1, -1, "One"), ("two.adx", -1, -1, "Two")])
            for name in ("one.adx", "two.adx"): Path(folder, name).write_bytes(b"old")
            def encode(args, **kwargs):
                if args[-2].endswith("two.adx") and fail[0]:
                    return SimpleNamespace(returncode=1, stderr="disk full", stdout="")
                Path(args[-1]).write_bytes(b"levelled")
                return SimpleNamespace(returncode=0, stderr="", stdout="ok")
            fail = [True]
            with mock.patch.object(soundtrack.subprocess, "run", side_effect=encode):
                self.assertEqual(soundtrack.level_album(folder, "encoder"), 1)
                self.assertNotIn("#level", Path(folder, "tracks.txt").read_text())
                self.assertEqual(Path(folder, "two.adx").read_bytes(), b"old")
                fail[0] = False
                self.assertEqual(soundtrack.level_album(folder, "encoder"), 2)
                self.assertIn("#level", Path(folder, "tracks.txt").read_text())
                self.assertEqual(soundtrack.level_album(folder, "encoder"), 0)

    def test_import_rejects_negative_wave_indices_and_sanitizes_cue_files(self):
        sound = b"KBDSbgm_stream" + bytes(100)
        source = mock.Mock()
        source.read.side_effect = lambda name: b"bank" if name.endswith(".xwb") else afs([sound])
        with tempfile.TemporaryDirectory() as folder, \
                mock.patch.object(soundtrack, "wave_bank", return_value=[(0, 4, 2, 48000, 1)]), \
                mock.patch.object(soundtrack, "cue_tracks", return_value=[("bad", -1, None), ("bgm_x/../evil\n", 0, None)]), \
                mock.patch.object(soundtrack, "decode", return_value=bytes(4)), \
                mock.patch.object(soundtrack, "import_movies"), \
                mock.patch.object(soundtrack, "level_album"), \
                mock.patch.object(soundtrack.subprocess, "run") as run:
            self.assertEqual(soundtrack.import_doa4(source, folder, "ffmpeg", None), 1)
            lines = Path(folder, "doa4", "tracks.txt").read_text().splitlines()
            self.assertEqual(len(lines), 3)
            self.assertTrue(lines[2].startswith("01_bgm_x____evil_.adx\t"))
            self.assertEqual(run.call_count, 1)

    def test_failed_manifest_replace_keeps_previous_file(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder, "tracks.txt")
            path.write_text("old")
            with mock.patch.object(soundtrack.os, "replace", side_effect=OSError("disk full")):
                with self.assertRaises(OSError): soundtrack.atomic_text(str(path), "new")
            self.assertEqual(path.read_text(), "old")
            self.assertEqual(list(Path(folder).iterdir()), [path])


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
