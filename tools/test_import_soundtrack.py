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


def wave_bank_image(waves, entry=0x40, bank=0x100, table=0x200, body=0x400, total=0x500):
    """A big-endian XACT 2 wave bank: waves [(fmt, offset into the wave data, length)]."""
    image = bytearray(total)
    image[:4] = b"DNBW"
    segments = ((bank, 0x80), (table, entry * len(waves)), (0, 0), (0, 0), (body, total - body))
    for i, (offset, size) in enumerate(segments):
        struct.pack_into(">II", image, 8 + 8 * i, offset, size)
    struct.pack_into(">I", image, bank + 4, len(waves))
    struct.pack_into(">I", image, bank + 0x48, entry)
    for i, (fmt, offset, length) in enumerate(waves):
        struct.pack_into(">4I", image, table + i * entry, 0, fmt, offset, length)
    return bytes(image)


def sound_bank_image(cues):
    """A big-endian XACT 2 sound bank: cues [(name, wave)] for a simple sound or
    [(name, [intro, loop])] for a complex one."""
    image = bytearray(0x400)
    image[:4] = b"KBDS"
    struct.pack_into(">h", image, 0x09, len(cues))
    struct.pack_into(">i", image, 0x1A, 0x40)  # the sounds' table
    struct.pack_into(">i", image, 0x3A, 0x80)  # the names' table
    sounds_at = 0x200
    for i, (label, waves) in enumerate(cues):
        name = 0x100 + 24 * i
        image[name:name + len(label)] = label.encode()
        struct.pack_into(">i", image, 0x40 + 5 * i + 1, sounds_at)
        struct.pack_into(">i", image, 0x80 + 6 * i, name)
        sound, sounds_at = sounds_at, sounds_at + 0x40
        if isinstance(waves, int):
            image[sound] = 0  # a simple sound: one wave, once
            struct.pack_into(">h", image, sound + 9, waves)
            continue
        image[sound] = 1  # a complex sound: one track per wave
        image[sound + 9] = len(waves)
        for k, wave in enumerate(waves):
            track = sound + 0x20 + 0x10 * k
            struct.pack_into(">i", image, sound + 10 + 5 * k + 1, track)
            image[track] = 1
            struct.pack_into(">h", image, track + 8, wave)
    return bytes(image)


def xbe_image(blobs, base=0x40000000):
    """An XBE whose data section holds blobs {offset in it: bytes}: a table's address
    is base + 0x400 + offset."""
    size = 0x400 + max(offset + len(blob) for offset, blob in blobs.items())
    data = bytearray(size)
    data[:4] = b"XBEH"
    struct.pack_into("<I", data, 0x104, base)
    struct.pack_into("<II", data, 0x11C, 2, base + 0x200)
    struct.pack_into("<IIIII", data, 0x200, 0, base, 0x400, 0, 0x400)
    struct.pack_into("<IIIII", data, 0x238, 0, base + 0x400, size - 0x400, 0x400, size - 0x400)
    for offset, blob in blobs.items():
        data[0x400 + offset:0x400 + offset + len(blob)] = blob
    return bytes(data)


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

    def test_doa1u_and_movie_names_come_from_their_codes(self):
        code, name = next(iter(soundtrack.DOA1_CODES.items()))
        self.assertEqual(soundtrack.doa1u_name(f"{code}_sng.adx"), name)
        self.assertEqual(soundtrack.doa1u_name(f"{code}_lose.adx"), name + " (Lose)")
        self.assertEqual(soundtrack.doa1u_name(f"{code}_lose2.adx"), name + " (Lose 2)")
        self.assertEqual(soundtrack.doa1u_name("xyz_sng.adx"), "Xyz Sng")
        code, name = next(iter(soundtrack.DOA3_CODES.items()))
        self.assertEqual(soundtrack.movie_name(f"mv_{code}.sfd"), f"{name}'s Ending")
        self.assertEqual(soundtrack.movie_name("op_movie.sfd"), "Op Movie")
        self.assertEqual(soundtrack.movie_name("GAME_G4.sfd"), "Game G4")


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

    def test_level_album_rejects_track_names_outside_the_album(self):
        with tempfile.TemporaryDirectory() as base:
            folder = os.path.join(base, "album")
            os.makedirs(folder)
            outside = Path(base, "outside.adx")
            outside.write_bytes(b"outside")
            with mock.patch.object(soundtrack.subprocess, "run") as run:
                for name in ("../outside.adx", str(outside.resolve()), "sub/outside.adx", ".", ".."):
                    Path(folder, "tracks.txt").write_text(
                        f"#title\tAlbum\n{name}\t-1\t-1\tEscape\n", encoding="utf-8")
                    with self.assertRaises(ValueError):
                        soundtrack.level_album(folder, "encoder")
            run.assert_not_called()
            self.assertEqual(outside.read_bytes(), b"outside")

    def test_only_a_missing_ffmpeg_is_reported_as_ffmpeg(self):
        source = mock.Mock()
        here = tempfile.gettempdir()
        ffmpeg = os.path.join(here, "ffmpeg.exe")
        encoder = os.path.join(here, "recomp_adx_encode.exe")
        scratch = os.path.join(here, "album", "01.adx.level")

        def message(error):
            with mock.patch.object(soundtrack.sys, "argv", ["import_soundtrack.py", "disc.iso"]), \
                    mock.patch.object(soundtrack, "open_source", return_value=source), \
                    mock.patch.object(soundtrack, "detect", return_value="doa4"), \
                    mock.patch.object(soundtrack, "find_ffmpeg", return_value=ffmpeg), \
                    mock.patch.object(soundtrack, "import_doa4", side_effect=error):
                with self.assertRaises(SystemExit) as caught:
                    soundtrack.main()
            return str(caught.exception)

        said = message(FileNotFoundError(2, "No such file", ffmpeg))
        self.assertIn("ffmpeg was not found", said)
        self.assertIn("--ffmpeg", said)
        for path in (encoder, scratch):
            said = message(FileNotFoundError(2, "No such file", path))
            self.assertIn(path, said)
            self.assertNotIn("ffmpeg", said)

    def test_missing_encoder_when_levelling_reports_its_path(self):
        with tempfile.TemporaryDirectory() as out:
            os.makedirs(os.path.join(out, "doa3"))
            Path(out, "doa3", "tracks.txt").write_text(
                "#title\tAlbum\n01.adx\t-1\t-1\tOne\n", encoding="utf-8")
            encoder = os.path.join(out, "recomp_adx_encode.exe")
            with mock.patch.object(soundtrack.sys, "argv", ["import_soundtrack.py", "--level", "--out", out]), \
                    mock.patch.object(soundtrack, "find_encoder", return_value=encoder):
                with self.assertRaises(SystemExit) as caught:
                    soundtrack.main()
            self.assertIn(encoder, str(caught.exception))
            self.assertNotIn("ffmpeg", str(caught.exception))

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

    def test_doa2u_bgm_afs_is_not_imported_as_doa3(self):
        with tempfile.TemporaryDirectory() as folder:
            doa2 = os.path.join(folder, "DOA2")
            os.makedirs(doa2)
            Path(doa2, "bgm.afs").write_bytes(b"AFS\0" + bytes(20))
            Path(doa2, "voice.afs").write_bytes(b"AFS\0" + bytes(20))
            # Bare DOA2/bgm.afs, or its folder, is DOA2U's music, not DOA3's album. So is the
            # archive when DOA1/Sound sits beside it, one folder up.
            for given in (os.path.join(doa2, "bgm.afs"), doa2):
                source = soundtrack.open_source(given)
                try:
                    self.assertEqual(soundtrack.detect(source), "doa2u")
                finally:
                    source.close()
            os.makedirs(os.path.join(folder, "extraction", "DOA1", "Sound"))
            os.makedirs(os.path.join(folder, "extraction", "DOA2"))
            Path(folder, "extraction", "DOA2", "bgm.afs").write_bytes(b"AFS\0" + bytes(20))
            given = os.path.join(folder, "extraction", "DOA2", "bgm.afs")
            source = soundtrack.open_source(given)
            try:
                self.assertEqual(soundtrack.detect(source), "doa2u")
            finally:
                source.close()
            with mock.patch.object(soundtrack.sys, "argv", ["import_soundtrack.py", given]), \
                    mock.patch.object(soundtrack, "import_doa2u") as doa2u, \
                    mock.patch.object(soundtrack, "import_doa3") as doa3:
                self.assertEqual(soundtrack.main(), 0)
            doa2u.assert_called_once()
            doa3.assert_not_called()

    def test_folders_and_images_list_their_movie_folders(self):
        adx = b"\x80\x00\x00\x20" + bytes(100)
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "disc.iso")
            xdvdfs_image(path, {"bgm.afs": afs([adx]), "SOUND/one.xwb": b"x"})
            disc = soundtrack.Xdvdfs(path)
            try:
                self.assertEqual(disc.list(""), ["bgm.afs"])
                self.assertEqual(disc.list("SOUND"), ["one.xwb"])
                self.assertEqual(disc.list("NOSUCH"), [])
            finally:
                disc.close()
            Path(folder, "movie").mkdir()
            Path(folder, "movie", "b.sfd").write_bytes(b"b")
            Path(folder, "movie", "A.sfd").write_bytes(b"a")
            Path(folder, "movie", "notes.txt").write_bytes(b"n")
            here = soundtrack.Folder(folder)
            self.assertEqual(here.list("movie"), ["A.sfd", "b.sfd", "notes.txt"])
            self.assertEqual(here.list("nosuch"), [])


def movie_image(payload):
    """A Sofdec movie holding one MPEG audio stream: a pack header, the stream, an end code."""
    pes = b"\0\0\1\xc0" + struct.pack(">H", 1 + len(payload)) + b"\x0f" + payload
    return b"\0\0\1\xba\x21" + bytes(7) + pes + b"\0\0\1\xb9"


class XbeTests(unittest.TestCase):
    BASE = 0x40000000
    # A base whose data section holds Doa1.xbe's music table at its own address.
    DOA1_BASE = 0x2DFC00

    def image(self, names, base=None, table=None):
        """An XBE with a pointer table and a name per entry after it."""
        base = self.BASE if base is None else base
        table = base + 0x400 if table is None else table
        entries, blobs, at = [], {table - (base + 0x400): b""}, 0x400
        for name in names:
            entries.append(base + 0x400 + at)
            blobs[at] = name.encode() + b"\0"
            at += 0x40
        blobs[table - (base + 0x400)] = struct.pack(f"<{len(entries) + 1}I", *entries, 0)
        return xbe_image(blobs, base)

    def test_music_table_lists_its_files_in_order(self):
        image = self.image(["aa_sng.adx", "bb_lose.adx"])
        self.assertEqual(soundtrack.xbe_music(image, self.BASE + 0x400), ["aa_sng.adx", "bb_lose.adx"])
        with self.assertRaises(ValueError):
            soundtrack.xbe_music(image, 0x10)  # outside the executable

    def test_doa1u_album_copies_the_executables_music(self):
        adx = b"\x80\x00\x00\x20" + bytes(100)
        with tempfile.TemporaryDirectory() as folder:
            sound = Path(folder, "DOA1", "Sound")
            sound.mkdir(parents=True)
            (sound / "aa_sng.adx").write_bytes(adx)
            (sound / "bb_lose.adx").write_bytes(adx + b"x")
            (sound / "zz_sng.adx").write_bytes(b"not an ADX")
            image = self.image(["aa_sng.adx", "bb_lose.adx", "zz_sng.adx", "../escape.adx"],
                               self.DOA1_BASE, soundtrack.DOA1_TABLE)
            Path(folder, "Doa1.xbe").write_bytes(image)
            out = os.path.join(folder, "out")
            self.assertEqual(soundtrack.import_doa1u(soundtrack.Folder(folder), out), 2)
            lines = Path(out, "doa1u", "tracks.txt").read_text(encoding="utf-8").splitlines()
            self.assertEqual(lines[0], "#title\tDead or Alive Ultimate")
            self.assertEqual(lines[2], "aa_sng.adx\t-1\t-1\tAa Sng")
            self.assertEqual(lines[3], "bb_lose.adx\t-1\t-1\tBb Lose")
            self.assertEqual(Path(out, "doa1u", "aa_sng.adx").read_bytes(), adx)
            self.assertFalse(Path(out, "doa1u", "zz_sng.adx").exists())

    def test_doa1u_album_without_the_executable_is_left_alone(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder, "out")
            (out / "doa1u").mkdir(parents=True)
            (out / "doa1u" / "tracks.txt").write_text("old", encoding="utf-8")
            self.assertEqual(soundtrack.import_doa1u(soundtrack.Folder(folder), str(out)), 0)
            self.assertEqual((out / "doa1u" / "tracks.txt").read_text(encoding="utf-8"), "old")

    def test_movies_are_every_sfd_in_the_folder(self):
        adx = b"\x80\x00\x00\x20" + bytes(100)
        code, fighter = next(iter(soundtrack.DOA3_CODES.items()))
        ending = f"mv_{code}.sfd"
        with tempfile.TemporaryDirectory() as folder:
            movies = Path(folder, "movie")
            movies.mkdir()
            (movies / "late.sfd").write_bytes(movie_image(adx + b"x"))
            (movies / ending).write_bytes(movie_image(adx))
            (movies / "same.sfd").write_bytes(movie_image(adx))  # its audio is a repeat
            (movies / "notes.txt").write_bytes(b"not a movie")
            out = Path(folder, "out")
            out.mkdir()
            tracks = []
            soundtrack.import_movies(soundtrack.Folder(folder), str(out), "movie", {}, tracks)
            self.assertEqual([t[0] for t in tracks], ["m_late.adx", f"m_mv_{code}.adx"])
            self.assertEqual([t[3] for t in tracks], ["Late", f"{fighter}'s Ending"])
            self.assertTrue(Path(out, f"m_mv_{code}.adx").is_file())


class BankTests(unittest.TestCase):
    XMA = 1 | (2 << 1) | (48000 << 4)  # XMA, stereo, 48 kHz

    def test_xact_banks_parse(self):
        self.assertEqual(soundtrack.wave_bank(wave_bank_image([(self.XMA, 8, 16)])),
                         [(0x400 + 8, 16, 2, 48000, 0)])
        self.assertEqual(
            soundtrack.cue_tracks(sound_bank_image([("bgm_one", 3), ("rmx_two", [1, 2])])),
            [("bgm_one", 3, None), ("rmx_two", 1, 2)])

    def test_corrupt_wave_bank_raises_value_error(self):
        good = wave_bank_image([(self.XMA, 0, 16)])
        self.assertRaises(ValueError, soundtrack.wave_bank, b"DNBW" + bytes(4))
        for at in (8, 16, 40):  # the bank header, the wave table and the wave data
            corrupt = bytearray(good)
            struct.pack_into(">I", corrupt, at, 0xFFFF0)
            self.assertRaises(ValueError, soundtrack.wave_bank, bytes(corrupt))
        self.assertRaises(ValueError, soundtrack.wave_bank, wave_bank_image([(self.XMA, 0xFFF0, 16)]))
        self.assertRaises(ValueError, soundtrack.wave_bank, wave_bank_image([(self.XMA, 0, 16)], entry=8))

    def test_corrupt_sound_bank_raises_value_error(self):
        self.assertRaises(ValueError, soundtrack.cue_tracks, b"KBDS" + bytes(8))
        good = bytearray(sound_bank_image([("bgm", 1)]))
        for at in (0x1A, 0x40 + 1, 0x80):  # the cue table, the cue's sound and its name
            corrupt = bytearray(good)
            struct.pack_into(">i", corrupt, at, 0xFFF0)
            self.assertRaises(ValueError, soundtrack.cue_tracks, bytes(corrupt))
        self.assertRaises(ValueError, soundtrack.cue_tracks, bytes(good[:0x10]))
        negative = bytearray(good)
        struct.pack_into(">h", negative, 0x09, -1)  # a negative cue count
        self.assertRaises(ValueError, soundtrack.cue_tracks, bytes(negative))
        corrupt = bytearray(sound_bank_image([("bgm", [1, 2])]))
        corrupt[0x200 + 9] = 200  # more tracks than the bank holds
        self.assertRaises(ValueError, soundtrack.cue_tracks, bytes(corrupt))


if __name__ == "__main__":
    unittest.main()
