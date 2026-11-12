#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode the fighters' hit grunts and KO cries from your own Dead or Alive 4.

DOA2U's fighters make no sound when hit. recomp-runtime/hit_voice.c plays
DOA4's instead, from clips this tool decodes out of the snd.afs on your
extracted DOA4 disc into private/doa4-voice/<prefix>/hit_NN.wav and
ko_NN.wav. Nothing it writes may be committed.

snd.afs (AFS, little-endian) holds a big-endian XACT 2 wave bank and sound
bank per character, the waves as XMA1. In each sound bank the cues
<prefix>_down_03a..m are the short hit grunts and <prefix>_down_04 onward the
longer knock-down cries, which serve as KO cries. Needs an ffmpeg with the
xma1 decoder (any ordinary build) on PATH or given with --ffmpeg.

    python tools/import_doa4_voices.py F:/DOA4U/extracted_ISO/snd.afs
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

# The 15 DOA2U fighters' DOA4 voice prefixes and their sound bank's AFS index;
# the wave bank is the entry before it.
BANKS = {
    "zac": 8, "tin": 10, "jan": 12, "ein": 14, "ryu": 16, "kas": 18, "gen": 20, "hel": 22,
    "leo": 24, "bas": 26, "lei": 28, "aya": 30, "tng": 32, "bay": 34, "hak": 44,
}


def afs_entries(path):
    with open(path, "rb") as file:
        head = file.read(8)
        if head[:3] != b"AFS":
            raise SystemExit(f"{path} is not an AFS archive")
        count = struct.unpack_from("<I", head, 4)[0]
        table = file.read(8 * count)
    return [struct.unpack_from("<II", table, 8 * i) for i in range(count)]


def wave_bank(data):
    """(offset, length, channels, rate) per wave of a big-endian XACT 2 bank."""
    if data[:4] != b"DNBW":
        raise ValueError("not a big-endian wave bank")
    segments = [struct.unpack_from(">II", data, 8 + 8 * i) for i in range(5)]
    bank = segments[0][0]
    count = struct.unpack_from(">I", data, bank + 4)[0]
    size = struct.unpack_from(">I", data, bank + 0x48)[0]
    waves = []
    for i in range(count):
        _, fmt, offset, length = struct.unpack_from(">4I", data, segments[1][0] + i * size)
        if fmt & 1 == 0:
            raise ValueError(f"wave {i} is not XMA")
        waves.append((segments[4][0] + offset, length, (fmt >> 1) & 7, (fmt >> 4) & 0x7FFFF))
    return waves


def cue_waves(data):
    """Cue name -> wave index for a big-endian XACT 2 sound bank's simple cues."""
    if data[:4] != b"KBDS":
        raise ValueError("not a big-endian sound bank")
    s16 = lambda at: struct.unpack_from(">h", data, at)[0]
    s32 = lambda at: struct.unpack_from(">i", data, at)[0]
    cues, simple, names = s16(0x09), s32(0x1A), s32(0x3A)
    out = {}
    for i in range(cues):
        sound, name = s32(simple + 5 * i + 1), s32(names + 6 * i)
        label = data[name:data.index(b"\0", name)].decode("ascii")
        if data[sound] & 1 == 0:  # a simple sound: one wave
            out[label] = s16(sound + 9)
    return out


def xma1_riff(raw, channels, rate):
    fmt = struct.pack("<HHHHHBB", 0x165, 16, 0x10D6, 0, 1, 0, 2)
    fmt += struct.pack("<IIIIBBH", rate * channels // 2, rate, 0, 0, 0, channels, 4 if channels == 1 else 3)
    body = b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(raw)) + raw
    return b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("snd_afs", help="snd.afs from your extracted DOA4 disc")
    parser.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "private", "doa4-voice"))
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()
    entries = afs_entries(args.snd_afs)
    with open(args.snd_afs, "rb") as afs, tempfile.TemporaryDirectory() as scratch:
        def read(index):
            offset, length = entries[index]
            afs.seek(offset)
            return afs.read(length)

        for prefix, index in BANKS.items():
            waves, cues = wave_bank(read(index - 1)), cue_waves(read(index))
            pools = {
                "hit": sorted({w for c, w in cues.items() if re.fullmatch(prefix + r"_down_03[a-z]", c)}),
                "ko": sorted({w for c, w in cues.items() if re.fullmatch(prefix + r"_down_(0[4-9]|1\d)[a-z]?", c)}),
            }
            folder = os.path.join(args.out, prefix)
            os.makedirs(folder, exist_ok=True)
            bank = read(index - 1)
            for kind, pool in pools.items():
                for n, wave in enumerate(pool):
                    offset, length, channels, rate = waves[wave]
                    source = os.path.join(scratch, "clip.wav")
                    with open(source, "wb") as file:
                        file.write(xma1_riff(bank[offset:offset + length], channels, rate))
                    subprocess.run([args.ffmpeg, "-v", "error", "-y", "-i", source, "-ac", "1", "-ar", "48000",
                        "-c:a", "pcm_s16le", os.path.join(folder, f"{kind}_{n:02d}.wav")], check=True)
            print(f"{prefix}: {len(pools['hit'])} hit, {len(pools['ko'])} KO clips")
    return 0


if __name__ == "__main__":
    sys.exit(main())
