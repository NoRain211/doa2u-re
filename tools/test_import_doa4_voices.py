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


def make_sound_bank(cues):
    """A big-endian XACT 2 sound bank holding one simple cue per (name, wave)."""
    simple, names = 0x40, 0x40 + 5 * len(cues)
    sounds = names + 6 * len(cues)
    labels = sounds + 16 * len(cues)
    text = bytearray()
    data = bytearray(labels)
    data[0:4] = b"KBDS"
    struct.pack_into(">h", data, 0x09, len(cues))
    struct.pack_into(">i", data, 0x1A, simple)
    struct.pack_into(">i", data, 0x3A, names)
    for i, (name, wave) in enumerate(cues):
        struct.pack_into(">i", data, simple + 5 * i + 1, sounds + 16 * i)
        struct.pack_into(">i", data, names + 6 * i, labels + len(text))
        text += name.encode("ascii") + b"\0"
        # Its flag byte stays 0: a simple sound, with the wave at +9.
        struct.pack_into(">h", data, sounds + 16 * i + 9, wave)
    return bytes(data + text)


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

    def test_invalid_banks_create_no_output_and_never_run_the_decoder(self):
        with tempfile.TemporaryDirectory() as root:
            source, out = os.path.join(root, "snd.afs"), os.path.join(root, "output")
            count = max(voices.BANKS.values()) + 1
            end = 8 + 8 * count
            with open(source, "wb") as file:
                file.write(b"AFS\0" + struct.pack("<I", count) + struct.pack("<II", end, 0) * count)
            with mock.patch.object(voices, "fetch_ffmpeg", return_value="unused"), \
                    mock.patch.object(voices.subprocess, "run") as run:
                with self.assertRaises((ValueError, struct.error)):
                    voices.main([source, "--out", out])
            self.assertFalse(os.path.exists(out))
            run.assert_not_called()


class BankTests(unittest.TestCase):
    def test_cue_names_that_could_leave_the_output_folder_are_rejected(self):
        for name in ("../../escape", "a/b", "a\\b", "..", "x..y", "", "sp ace", "\x01no"):
            with self.subTest(name=name):
                with self.assertRaises(ValueError):
                    voices.cue_waves(make_sound_bank([(name, 0)]))

    def test_safe_cue_names_map_to_their_wave(self):
        self.assertEqual(voices.cue_waves(make_sound_bank([("zac_068_down_03a", 3), ("zac_071_down_03d", 5)])),
                         {"zac_068_down_03a": 3, "zac_071_down_03d": 5})

    def test_a_poisoned_cue_name_writes_nothing(self):
        with tempfile.TemporaryDirectory() as root:
            wave = make_wave_bank([(1, 48000, 4)])
            afs, entries = voice_entries(wave, make_sound_bank([("../../escaped", 0)]))
            out = os.path.join(root, "out")
            with mock.patch.object(voices.subprocess, "run") as run:
                with self.assertRaises(ValueError):
                    voices.decode_bank(afs, entries, "zac", out, "ffmpeg")
            run.assert_not_called()
            self.assertFalse(os.path.exists(out))
            self.assertEqual(os.listdir(root), [])

    def test_a_valid_bank_still_parses(self):
        self.assertEqual(voices.wave_bank(make_wave_bank([(1, 48000, 4), (2, 48000, 8)])),
                         [(0x200, 4, 1, 48000), (0x204, 8, 2, 48000)])
        afs, entries = voice_entries(make_wave_bank([(1, 48000, 4)]), make_sound_bank([("zac_068_down", 0)]))
        with tempfile.TemporaryDirectory() as out, mock.patch.object(voices.subprocess, "run") as run:
            files = voices.decode_bank(afs, entries, "zac", out, "ffmpeg")
        self.assertEqual(files, {0: "zac_000_068_down.wav"})
        run.assert_called_once()

    def test_a_malformed_wave_table_is_reported_not_looped_or_indexed(self):
        bank = make_wave_bank([(1, 48000, 4)])
        for header, count, size in ((0, 8, 0), (0, 1000, 16), (0xFFFFFFFF, 1, 16)):
            with self.subTest(header=header, count=count, size=size):
                data = bytearray(bank)
                struct.pack_into(">II", data, 8, header, 0x100)
                struct.pack_into(">I", data, 4, count)
                struct.pack_into(">I", data, 0x48, size)
                with self.assertRaises(ValueError):
                    voices.wave_bank(bytes(data))

    def test_a_short_entry_table_is_reported_not_indexed(self):
        with self.assertRaises(ValueError):
            voices.validate_banks(voices.ArchiveView(io.BytesIO(b""), 0, 0), [])

    def test_the_source_is_checked_before_ffmpeg_is_fetched(self):
        with tempfile.TemporaryDirectory() as root:
            source, out = os.path.join(root, "snd.afs"), os.path.join(root, "output")
            count = max(voices.BANKS.values()) + 1
            with open(source, "wb") as file:
                file.write(b"AFS\0" + struct.pack("<I", count) + struct.pack("<II", 0, 0) * count)
            with mock.patch.object(voices, "fetch_ffmpeg",
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


if __name__ == "__main__":
    unittest.main()
