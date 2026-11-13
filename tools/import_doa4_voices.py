#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode every voice clip of the fighters from your own Dead or Alive 4.

DOA2U's fighters make no sound when hit. recomp-runtime/hit_voice.c plays
DOA4 clips instead, the ones you choose. This tool decodes every wave of the
15 fighters' voice banks from the snd.afs on your extracted DOA4 disc into
private/doa4-voice-all/<prefix>/<prefix>_NNN_<cue>.wav, named after the
first cue that plays it, with index.csv listing each clip's length and cues.
Then it builds each fighter's <prefix>/selections/ from the mapping in
tools/doa4_voice_selections.json (which clip plays as a light, medium or heavy
hit or a KO, and how often), so every install gets the same voices from its
own disc; folders that already hold clips are kept. Your own picks go in
<prefix>/selections/ as <name>_HIT_LV1_NN.wav (light hits), _HIT_LV2_
(medium), _HIT_LV3_ (heavy) and <name>_KO_NN.wav; --save-selections writes
them back into the mapping. A fighter without selections stays silent.
Nothing it writes into private/ may be committed; the mapping holds no audio. Needs an ffmpeg with the xma1 decoder
(any ordinary build) on PATH or given with --ffmpeg.

    python tools/import_doa4_voices.py F:/DOA4U/extracted_ISO/snd.afs
    python tools/import_doa4_voices.py F:/DOA4U/extracted_ISO/snd.afs --save-selections
"""
import argparse
import csv
import json
import os
import re
import shutil
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


MANIFEST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "doa4_voice_selections.json")
FIGHTER_NAMES = {
    "aya": "Ayane", "bas": "Bass", "bay": "Bayman", "ein": "Ein", "gen": "Gen Fu", "hel": "Helena",
    "jan": "Jann Lee", "kas": "Kasumi", "kor": "Hitomi", "lei": "Leifang", "leo": "Leon",
    "ryu": "Ryu Hayabusa", "tin": "Tina", "tng": "Tengu", "zac": "Zack",
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


def decode_bank(afs, entries, prefix, out_folder, ffmpeg, rows=None, only=None):
    """Decodes a fighter's waves (all, or the indices in `only`) into out_folder;
    returns {wave index: file name}."""
    def read(index):
        offset, length = entries[index]
        afs.seek(offset)
        return afs.read(length)

    index = BANKS[prefix]
    bank = read(index - 1)
    waves, cues = wave_bank(bank), cue_waves(read(index))
    names = {}
    for cue, wave in cues.items():
        names.setdefault(wave, []).append(cue[len(prefix) + 1:] if cue.startswith(prefix + "_") else cue)
    os.makedirs(out_folder, exist_ok=True)
    files = {}
    with tempfile.TemporaryDirectory() as scratch:
        for n, (offset, length, channels, rate) in enumerate(waves):
            if only is not None and n not in only:
                continue
            labels = sorted(names.get(n, []))
            source = os.path.join(scratch, "clip.wav")
            with open(source, "wb") as file:
                file.write(xma1_riff(bank[offset:offset + length], channels, rate))
            name = f"{prefix}_{n:03d}" + (f"_{labels[0]}" if labels else "") + ".wav"
            target = os.path.join(out_folder, name)
            subprocess.run([ffmpeg, "-v", "error", "-y", "-i", source, "-ac", "1", "-ar", "48000",
                "-c:a", "pcm_s16le", target], check=True)
            files[n] = name
            if rows is not None:
                frames = (os.path.getsize(target) - 44) // 2
                rows.append([prefix, n, name, f"{frames / 48000:.2f}", " ".join(labels)])
    return files


def pcm_of(path):
    """A WAV's sample bytes (its data chunk)."""
    with open(path, "rb") as file:
        data = file.read()
    at = 12
    while at + 8 <= len(data):
        chunk, size = data[at:at + 4], struct.unpack_from("<I", data, at + 4)[0]
        if chunk == b"data":
            return data[at + 8:at + 8 + size]
        at += 8 + size + (size & 1)
    return b""


def same_clip(a, b):
    """Equal, or equal but for rounding (VoiceEditor rewrites the clips it adds)."""
    if a == b:
        return True
    if len(a) != len(b) or len(a) < 2:
        return False
    count = len(a) // 2
    x, y = struct.unpack(f"<{count}h", a[:2 * count]), struct.unpack(f"<{count}h", b[:2 * count])
    return all(abs(p - q) <= 2 for p, q in zip(x, y))


def read_tuning(path):
    """voice.ini's key=value lines, in order."""
    tuning = {}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as file:
            for line in file:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    key, value = line.split("=", 1)
                    tuning[key.strip()] = value.strip()
    return tuning


def selection_files(folder):
    if not os.path.isdir(folder):
        return []
    return sorted(name for name in os.listdir(folder) if name.lower().endswith(".wav"))


def apply_selections(afs, entries, out, ffmpeg, manifest, replace):
    """Builds each fighter's selections folder from the mapping, from the waves just decoded."""
    for prefix, fighter in sorted(manifest.get("fighters", {}).items()):
        if prefix not in BANKS:
            continue
        folder = os.path.join(out, prefix, "selections")
        if selection_files(folder) and not replace:
            print(f"{prefix}: keeping your own selections (--replace-selections to rebuild them)")
            continue
        os.makedirs(folder, exist_ok=True)
        if replace:
            for name in selection_files(folder):
                os.remove(os.path.join(folder, name))
        source = os.path.join(out, prefix)
        decoded = {}
        for name in os.listdir(source):
            match = re.match(re.escape(prefix) + r"_(\d{3})", name)
            if match:
                decoded[int(match.group(1))] = name
        missing = {clip["wave"] for clip in fighter["clips"] if clip["wave"] not in decoded}
        if missing:
            decoded.update(decode_bank(afs, entries, prefix, source, ffmpeg, only=missing))
        for clip in fighter["clips"]:
            shutil.copyfile(os.path.join(source, decoded[clip["wave"]]), os.path.join(folder, clip["file"]))
        if fighter.get("tuning"):
            with open(os.path.join(folder, "voice.ini"), "w", encoding="utf-8", newline="\n") as file:
                file.write(f"# {fighter.get('name', prefix)}'s hit voices; from {os.path.basename(MANIFEST)}.\n")
                for key, value in fighter["tuning"].items():
                    file.write(f"{key}={value}\n")
        print(f"{prefix}: {len(fighter['clips'])} selected clips")


def format_mapping(fighters):
    """The mapping as JSON, one clip a line."""
    lines = ['{', '  "version": 1,', '  "fighters": {']
    for i, (prefix, entry) in enumerate(sorted(fighters.items())):
        lines.append(f'    {json.dumps(prefix)}: {{')
        lines.append(f'      "name": {json.dumps(entry["name"])},')
        if entry.get("tuning"):
            lines.append(f'      "tuning": {json.dumps(entry["tuning"])},')
        lines.append('      "clips": [')
        clips = entry["clips"]
        for j, clip in enumerate(clips):
            lines.append("        " + json.dumps(clip) + ("," if j + 1 < len(clips) else ""))
        lines.append('      ]')
        lines.append('    }' + ("," if i + 1 < len(fighters) else ""))
    lines += ['  }', '}', '']
    return "\n".join(lines)


def save_selections(afs, entries, out, ffmpeg, manifest_path):
    """Writes the mapping from the selections folders: each clip's wave, found by its sound."""
    fighters = {}
    unknown = []
    with tempfile.TemporaryDirectory() as scratch:
        for prefix in sorted(BANKS):
            folder = os.path.join(out, prefix, "selections")
            chosen = selection_files(folder)
            if not chosen:
                continue
            files = decode_bank(afs, entries, prefix, os.path.join(scratch, prefix), ffmpeg)
            waves = [(n, pcm_of(os.path.join(scratch, prefix, name))) for n, name in sorted(files.items())]
            clips = []
            for name in chosen:
                sound = pcm_of(os.path.join(folder, name))
                wave = next((n for n, pcm in waves if same_clip(sound, pcm)), None)
                if wave is None:
                    unknown.append(f"{prefix}/selections/{name}")
                    continue
                clips.append({"file": name, "wave": wave, "cue": files[wave][len(prefix) + 5:-4]})
            entry = {"name": FIGHTER_NAMES.get(prefix, prefix), "clips": clips}
            tuning = read_tuning(os.path.join(folder, "voice.ini"))
            if tuning:
                entry["tuning"] = tuning
            fighters[prefix] = entry
            print(f"{prefix}: {len(clips)} clips mapped")
    with open(manifest_path, "w", encoding="utf-8", newline="\n") as file:
        file.write(format_mapping(fighters))
    if unknown:
        print("Not from DOA4's voice banks, so left out of the mapping:\n  " + "\n  ".join(unknown))
    print(f"Wrote {manifest_path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("snd_afs", help="snd.afs from your extracted DOA4 disc")
    parser.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "private", "doa4-voice-all"))
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--no-selections", action="store_true", help="decode only; don't build selections from the mapping")
    parser.add_argument("--replace-selections", action="store_true", help="rebuild selections folders that already have clips")
    parser.add_argument("--save-selections", action="store_true",
        help="write the mapping from your selections folders instead of decoding")
    parser.add_argument("--mapping", default=MANIFEST, help="the mapping file (default tools/doa4_voice_selections.json)")
    args = parser.parse_args()
    entries = afs_entries(args.snd_afs)
    with open(args.snd_afs, "rb") as afs:
        if args.save_selections:
            save_selections(afs, entries, args.out, args.ffmpeg, args.mapping)
            return 0
        rows = []
        for prefix in sorted(BANKS):
            files = decode_bank(afs, entries, prefix, os.path.join(args.out, prefix), args.ffmpeg, rows)
            print(f"{prefix}: {len(files)} clips")
        with open(os.path.join(args.out, "index.csv"), "w", newline="") as file:
            writer = csv.writer(file)
            writer.writerow(["character", "wave", "file", "seconds", "cues"])
            writer.writerows(rows)
        if not args.no_selections and os.path.exists(args.mapping):
            with open(args.mapping, encoding="utf-8") as file:
                apply_selections(afs, entries, args.out, args.ffmpeg, json.load(file), args.replace_selections)
    return 0

if __name__ == "__main__":
    sys.exit(main())
