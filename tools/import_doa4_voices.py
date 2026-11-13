#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode every voice clip of the fighters from your own Dead or Alive 4.

DOA2U's fighters make no sound when hit. recomp-runtime/hit_voice.c plays
DOA4 clips instead, the ones you choose. This tool decodes every wave of the
15 fighters' voice banks from the snd.afs on your extracted DOA4 disc into
private/doa4-voice-all/<prefix>/<prefix>_NNN_<cue>.wav, named after the
first cue that plays it, with index.csv listing each clip's length and cues.
Copy or rename the ones you want into <prefix>/selections/ as
<name>_HIT_LV1_NN.wav (light hits), _HIT_LV2_ (medium), _HIT_LV3_ (heavy) and
<name>_KO_NN.wav; a fighter without a selections folder stays silent.
Nothing it writes may be committed. Needs an ffmpeg with the xma1 decoder
(any ordinary build) on PATH or given with --ffmpeg.

    python tools/import_doa4_voices.py F:/DOA4U/extracted_ISO/snd.afs
"""
import argparse
import csv
import os
import struct
import subprocess
import sys
import tempfile

# The 15 DOA2U fighters' DOA4 voice prefixes and their sound bank's AFS index;
# the wave bank is the entry before it.
BANKS = {
    "zac": 8, "tin": 10, "jan": 12, "ein": 14, "ryu": 16, "kas": 18, "gen": 20, "hel": 22,
    "leo": 24, "bas": 26, "lei": 28, "aya": 30, "tng": 32, "bay": 34, "kor": 36,
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
    parser.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "private", "doa4-voice-all"))
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()
    entries = afs_entries(args.snd_afs)
    rows = []
    with open(args.snd_afs, "rb") as afs, tempfile.TemporaryDirectory() as scratch:
        def read(index):
            offset, length = entries[index]
            afs.seek(offset)
            return afs.read(length)

        for prefix, index in sorted(BANKS.items()):
            bank = read(index - 1)
            waves, cues = wave_bank(bank), cue_waves(read(index))
            names = {}
            for cue, wave in cues.items():
                names.setdefault(wave, []).append(cue[len(prefix) + 1:] if cue.startswith(prefix + "_") else cue)
            folder = os.path.join(args.out, prefix)
            os.makedirs(folder, exist_ok=True)
            for n, (offset, length, channels, rate) in enumerate(waves):
                labels = sorted(names.get(n, []))
                source = os.path.join(scratch, "clip.wav")
                with open(source, "wb") as file:
                    file.write(xma1_riff(bank[offset:offset + length], channels, rate))
                name = f"{prefix}_{n:03d}" + (f"_{labels[0]}" if labels else "") + ".wav"
                target = os.path.join(folder, name)
                subprocess.run([args.ffmpeg, "-v", "error", "-y", "-i", source, "-ac", "1", "-ar", "48000",
                    "-c:a", "pcm_s16le", target], check=True)
                frames = (os.path.getsize(target) - 44) // 2
                rows.append([prefix, n, name, f"{frames / 48000:.2f}", " ".join(labels)])
            print(f"{prefix}: {len(waves)} clips")
    with open(os.path.join(args.out, "index.csv"), "w", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(["character", "wave", "file", "seconds", "cues"])
        writer.writerows(rows)
    return 0

if __name__ == "__main__":
    sys.exit(main())
