# SPDX-License-Identifier: GPL-3.0-or-later
import json
import os
import struct
import tempfile
import unittest

import import_doa4_voices as voices


def wav(path, samples):
    data = struct.pack(f"<{len(samples)}h", *samples)
    fmt = struct.pack("<HHIIHH", 1, 1, 48000, 96000, 2, 16)
    with open(path, "wb") as file:
        file.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<I", 16) + fmt +
            b"data" + struct.pack("<I", len(data)) + data)


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
