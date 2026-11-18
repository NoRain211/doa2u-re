# SPDX-License-Identifier: GPL-3.0-or-later
import struct
import unittest

import xact


def afs_table_image(entries):
    """An AFS archive's header and table, without its file data: entries [(offset, length)]."""
    head = b"AFS\0" + struct.pack("<I", len(entries))
    for offset, length in entries:
        head += struct.pack("<II", offset, length)
    return head


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


class BankTests(unittest.TestCase):
    XMA = 1 | (2 << 1) | (48000 << 4)  # XMA, stereo, 48 kHz

    def test_xact_banks_parse(self):
        self.assertEqual(xact.wave_bank(wave_bank_image([(self.XMA, 8, 16)])),
                         [(0x400 + 8, 16, 2, 48000, 0)])
        self.assertEqual(
            xact.cue_tracks(sound_bank_image([("bgm_one", 3), ("rmx_two", [1, 2])])),
            [("bgm_one", 3, None, False), ("rmx_two", 1, 2, True)])

    def test_a_one_wave_complex_cue_is_marked(self):
        # A simple sound and a complex sound's only track both have no loop: the marker tells them apart.
        simple, one_wave = xact.cue_tracks(sound_bank_image([("simple", 3), ("one", [7])]))
        self.assertEqual((simple.name, simple.intro, simple.loop, simple.complex), ("simple", 3, None, False))
        self.assertEqual((one_wave.name, one_wave.intro, one_wave.loop, one_wave.complex), ("one", 7, None, True))

    def test_corrupt_wave_bank_raises_value_error(self):
        good = wave_bank_image([(self.XMA, 0, 16)])
        self.assertRaises(ValueError, xact.wave_bank, b"DNBW" + bytes(4))
        for at in (8, 16, 40):  # the bank header, the wave table and the wave data
            corrupt = bytearray(good)
            struct.pack_into(">I", corrupt, at, 0xFFFF0)
            self.assertRaises(ValueError, xact.wave_bank, bytes(corrupt))
        self.assertRaises(ValueError, xact.wave_bank, wave_bank_image([(self.XMA, 0xFFF0, 16)]))
        self.assertRaises(ValueError, xact.wave_bank, wave_bank_image([(self.XMA, 0, 16)], entry=8))
        many = bytearray(wave_bank_image([(self.XMA, 0, 16)]))
        struct.pack_into(">I", many, 0x100 + 4, 0x1000)  # more waves than the 0x500-byte bank holds
        self.assertRaises(ValueError, xact.wave_bank, bytes(many))

    def test_corrupt_sound_bank_raises_value_error(self):
        self.assertRaises(ValueError, xact.cue_tracks, b"KBDS" + bytes(8))
        good = bytearray(sound_bank_image([("bgm", 1)]))
        for at in (0x1A, 0x40 + 1, 0x80):  # the cue table, the cue's sound and its name
            corrupt = bytearray(good)
            struct.pack_into(">i", corrupt, at, 0xFFF0)
            self.assertRaises(ValueError, xact.cue_tracks, bytes(corrupt))
        self.assertRaises(ValueError, xact.cue_tracks, bytes(good[:0x10]))
        for at, value in ((0x40 + 1, -4), (0x80, -1)):  # a negative sound or name offset
            corrupt = bytearray(good)
            struct.pack_into(">i", corrupt, at, value)
            self.assertRaises(ValueError, xact.cue_tracks, bytes(corrupt))
        negative = bytearray(good)
        struct.pack_into(">h", negative, 0x09, -1)  # a negative cue count
        self.assertRaises(ValueError, xact.cue_tracks, bytes(negative))
        corrupt = bytearray(sound_bank_image([("bgm", [1, 2])]))
        corrupt[0x200 + 9] = 200  # more tracks than the bank holds
        self.assertRaises(ValueError, xact.cue_tracks, bytes(corrupt))

    def test_corrupt_afs_archive_raises_value_error(self):
        self.assertRaises(ValueError, xact.afs_table, None)
        self.assertRaises(ValueError, xact.afs_table, b"AFS\0")
        self.assertRaises(ValueError, xact.afs_table, b"AFS\0" + struct.pack("<I", 2) + bytes(8))
        self.assertEqual(xact.afs_table(afs_table_image([(2048, 4), (4096, 8)])), [(2048, 4), (4096, 8)])


if __name__ == "__main__":
    unittest.main()
