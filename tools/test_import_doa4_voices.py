# SPDX-License-Identifier: GPL-3.0-or-later
import io
import json
import os
import struct
import tempfile
import unittest
from unittest import mock
from test_import_soundtrack import xdvdfs_image

import import_doa4_voices as voices
import import_soundtrack as soundtrack
from test_xact import sound_bank_image


def wav(path, samples):
    data = struct.pack(f"<{len(samples)}h", *samples)
    fmt = struct.pack("<HHIIHH", 1, 1, 48000, 96000, 2, 16)
    with open(path, "wb") as file:
        file.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<I", 16) + fmt +
            b"data" + struct.pack("<I", len(data)) + data)


def make_wave_bank(clips):
    """A big-endian XACT 2 wave bank holding one XMA wave per (channels, rate, bytes)."""
    data = bytearray(0x200 + sum(length for _, _, length in clips))
    data[0:4] = b"DNBW"
    struct.pack_into(">II", data, 8, 0, 0x100)                        # bank header
    struct.pack_into(">II", data, 16, 0x100, 16 * len(clips))         # wave records
    struct.pack_into(">II", data, 24, 0, 0)
    struct.pack_into(">II", data, 32, 0, 0)
    struct.pack_into(">II", data, 40, 0x200, len(data) - 0x200)       # wave data
    struct.pack_into(">I", data, 4, len(clips))
    struct.pack_into(">I", data, 0x48, 16)
    at = 0
    for i, (channels, rate, length) in enumerate(clips):
        fmt = 1 | (channels << 1) | (rate << 4)
        struct.pack_into(">4I", data, 0x100 + 16 * i, 0, fmt, at, length)
        at += length
    return bytes(data)


def voice_entries(wave_bank, sound_bank):
    """An AFS entry table whose zac bank entries point at the two banks, read as one archive."""
    data = wave_bank + sound_bank
    entries = [(0, 0)] * (max(voices.BANKS.values()) + 1)
    entries[voices.BANKS["zac"] - 1] = (0, len(wave_bank))
    entries[voices.BANKS["zac"]] = (len(wave_bank), len(sound_bank))
    return voices.ArchiveView(io.BytesIO(data), 0, len(data)), entries


class ArchiveTests(unittest.TestCase):
    def test_archive_view_is_bounded_even_when_the_disc_has_following_files(self):
        view = voices.ArchiveView(io.BytesIO(b"xxvoice-tail"), 2, 5)
        self.assertEqual(view.read(99), b"voice")
        self.assertEqual(view.read(1), b"")
        view.seek(-2, 2)
        self.assertEqual(view.read(), b"ce")
        for offset, whence in [(-1, 0), (6, 0), (0, 7)]:
            with self.assertRaises(ValueError): view.seek(offset, whence)
        with self.assertRaises(ValueError): voices.ArchiveView(io.BytesIO(b"xxv"), 2, 5).read()

    def test_iso_voice_archive_is_read_without_extracting_the_disc(self):
        with tempfile.TemporaryDirectory() as root:
            iso = os.path.join(root, "doa4.iso")
            xdvdfs_image(iso, {"snd.afs": b"AFS\0voice", "SOUND/bgm_stream.xwb": b"DNBW"})
            with voices.open_archive(iso) as archive:
                self.assertEqual(archive.read(), b"AFS\0voice")
                self.assertEqual(archive.read(), b"")
            self.assertEqual(os.listdir(root), ["doa4.iso"])

    def test_a_bad_entry_table_is_reported_before_any_bank_is_read(self):
        for image in (b"nope", b"AFS\0" + struct.pack("<I", 1) + struct.pack("<II", 2048, 8)):
            with self.subTest(image=image):
                with self.assertRaises(ValueError):
                    voices.read_afs_entries(voices.ArchiveView(io.BytesIO(image), 0, len(image)))

    def test_invalid_banks_create_no_output_and_never_run_the_decoder(self):
        with tempfile.TemporaryDirectory() as root:
            source, out = os.path.join(root, "snd.afs"), os.path.join(root, "output")
            count = max(voices.BANKS.values()) + 1
            end = 8 + 8 * count
            with open(source, "wb") as file:
                file.write(b"AFS\0" + struct.pack("<I", count) + struct.pack("<II", end, 0) * count)
            with mock.patch.object(soundtrack, "fetch_ffmpeg", return_value="unused"), \
                    mock.patch.object(voices.subprocess, "run") as run:
                with self.assertRaises(ValueError):
                    voices.main([source, "--out", out])
            self.assertFalse(os.path.exists(out))
            run.assert_not_called()


class BankTests(unittest.TestCase):
    def test_a_poisoned_cue_name_stays_in_the_output_folder(self):
        with tempfile.TemporaryDirectory() as root:
            wave = make_wave_bank([(1, 48000, 4)])
            afs, entries = voice_entries(wave, sound_bank_image([("../../escaped", 0)]))
            out = os.path.join(root, "out")
            with mock.patch.object(voices.subprocess, "run") as run:
                files = voices.decode_bank(afs, entries, "zac", out, "ffmpeg")
            self.assertEqual(files, {0: "zac_000_______escaped.wav"})
            self.assertEqual(run.call_args.args[0][-1], os.path.join(out, files[0]))
            self.assertEqual(os.listdir(root), ["out"])

    def test_a_valid_bank_is_decoded(self):
        afs, entries = voice_entries(make_wave_bank([(1, 48000, 4)]), sound_bank_image([("zac_068_down", 0)]))
        with tempfile.TemporaryDirectory() as out, mock.patch.object(voices.subprocess, "run") as run:
            files = voices.decode_bank(afs, entries, "zac", out, "ffmpeg")
        self.assertEqual(files, {0: "zac_000_068_down.wav"})
        run.assert_called_once()

    def test_a_short_entry_table_is_reported_not_indexed(self):
        with self.assertRaises(ValueError):
            voices.validate_banks(voices.ArchiveView(io.BytesIO(b""), 0, 0), [])

    def test_the_source_is_checked_before_ffmpeg_is_fetched(self):
        with tempfile.TemporaryDirectory() as root:
            source, out = os.path.join(root, "snd.afs"), os.path.join(root, "output")
            count = max(voices.BANKS.values()) + 1
            with open(source, "wb") as file:
                file.write(b"AFS\0" + struct.pack("<I", count) + struct.pack("<II", 0, 0) * count)
            with mock.patch.object(soundtrack, "fetch_ffmpeg",
                                   side_effect=AssertionError("ffmpeg was fetched before the disc was checked")), \
                    mock.patch.object(voices.subprocess, "run") as run:
                with self.assertRaises(ValueError):
                    voices.main([source, "--out", out])
            run.assert_not_called()
            self.assertFalse(os.path.exists(out))


class MappingTests(unittest.TestCase):
    def test_committed_mapping_names_known_fighters_and_kinds(self):
        with open(voices.MANIFEST, encoding="utf-8") as file:
            mapping = json.load(file)
        self.assertEqual(mapping["version"], 1)
        for prefix, fighter in mapping["fighters"].items():
            self.assertIn(prefix, voices.BANKS)
            for clip in fighter["clips"]:
                self.assertRegex(clip["file"], r"_(HIT_LV[123]|KO)_\d+\.wav$")
                self.assertIsInstance(clip["wave"], int)
        self.assertEqual(json.loads(voices.format_mapping(mapping["fighters"])), mapping)

    def test_same_clip_allows_rounding_only(self):
        a = struct.pack("<4h", 0, 100, -100, 32767)
        self.assertTrue(voices.same_clip(a, a))
        self.assertTrue(voices.same_clip(a, struct.pack("<4h", 1, 99, -102, 32766)))
        self.assertFalse(voices.same_clip(a, struct.pack("<4h", 0, 120, -100, 32767)))
        self.assertFalse(voices.same_clip(a, a[:6]))

    def test_apply_builds_empty_selections_and_keeps_your_own(self):
        with tempfile.TemporaryDirectory() as out:
            os.makedirs(os.path.join(out, "aya"))
            wav(os.path.join(out, "aya", "aya_068_down_03a.wav"), [1, 2, 3])
            wav(os.path.join(out, "aya", "aya_071_down_03d.wav"), [4, 5])
            os.makedirs(os.path.join(out, "zac", "selections"))
            wav(os.path.join(out, "zac", "selections", "mine_KO_01.wav"), [9])
            mapping = {"fighters": {
                "aya": {"name": "Ayane", "tuning": {"gap_ms": "700"}, "clips": [
                    {"file": "ayane_HIT_LV1_01.wav", "wave": 68}, {"file": "ayane_KO_01.wav", "wave": 71},
                    {"file": "ayane_KO_02.wav", "wave": 71}]},
                "zac": {"name": "Zack", "clips": [{"file": "zac_KO_01.wav", "wave": 86}]},
            }}
            voices.apply_selections(None, None, out, "ffmpeg", mapping, False)
            chosen = os.path.join(out, "aya", "selections")
            self.assertEqual(voices.selection_files(chosen), ["ayane_HIT_LV1_01.wav", "ayane_KO_01.wav", "ayane_KO_02.wav"])
            self.assertEqual(voices.pcm_of(os.path.join(chosen, "ayane_KO_02.wav")), struct.pack("<2h", 4, 5))
            self.assertEqual(voices.read_tuning(os.path.join(chosen, "voice.ini")), {"gap_ms": "700"})
            self.assertEqual(voices.selection_files(os.path.join(out, "zac", "selections")), ["mine_KO_01.wav"])


class CueTests(unittest.TestCase):
    def test_only_simple_cues_map_to_waves(self):
        bank = sound_bank_image([("left_hit", 3), ("theme", [1, 2]), ("right_hit", 9)])
        self.assertEqual(voices.cue_waves(bank), {"left_hit": 3, "right_hit": 9})

    def test_cue_names_become_safe_file_names(self):
        # DOA4's own Tina bank has "tin_y01_tag dead"; the import must not stop on it.
        bank = sound_bank_image([("tin_y01_tag dead", 0), ("tin_hit", 1)])
        self.assertEqual(voices.cue_waves(bank), {"tin_y01_tag dead": 0, "tin_hit": 1})
        with tempfile.TemporaryDirectory() as root:
            afs, entries = voice_entries(make_wave_bank([(1, 48000, 4)]), sound_bank_image([("tin_y01_tag dead", 0)]))
            with mock.patch.object(voices.subprocess, "run"):
                self.assertEqual(voices.decode_bank(afs, entries, "zac", root, "ffmpeg"),
                                 {0: "zac_000_tin_y01_tag_dead.wav"})
        for name in ("../../escape", "a/b", "a\\b", "..", "x..y", "sp ace", "\x01no"):
            with self.subTest(name=name):
                self.assertRegex(voices.safe_cue(name), r"^[A-Za-z0-9_-]+$")


class AfsTests(unittest.TestCase):
    def test_an_entry_count_larger_than_the_file_raises_before_reading(self):
        def entries(image):
            return voices.read_afs_entries(voices.ArchiveView(io.BytesIO(image), 0, len(image)))

        with self.assertRaises(ValueError):
            entries(b"AFS\0" + struct.pack("<I", 0xFFFFFFFF) + bytes(8))
        self.assertEqual(entries(b"AFS\0" + struct.pack("<III", 1, 16, 4) + bytes(4)), [(16, 4)])


if __name__ == "__main__":
    unittest.main()
