# SPDX-License-Identifier: GPL-3.0-or-later
"""The AFS archives and big-endian XACT 2 banks on Dead or Alive 3 and 4's discs.

The soundtrack and the voice importers read the same containers, so their bounds-checked
parsers live here and a corrupt file is a ValueError rather than a struct.error.
"""
import struct
from collections import namedtuple


# ---- AFS archives -------------------------------------------------------------

def afs_table(data):
    """(offset, length) per file of an AFS archive."""
    if not data or data[:3] != b"AFS":
        raise ValueError("not an AFS archive")
    if len(data) < 8:
        raise ValueError("truncated AFS archive header")
    count = struct.unpack_from("<I", data, 4)[0]
    if len(data) < 8 + 8 * count:
        raise ValueError(f"AFS archive lists {count} files but is only {len(data)} bytes")
    return [struct.unpack_from("<II", data, 8 + 8 * i) for i in range(count)]


# ---- XACT 2 (Xbox 360, big-endian) --------------------------------------------

def be_at(data, at, fmt, what):
    """struct.unpack_from in a big-endian bank, refusing an offset outside the file."""
    size = struct.calcsize(fmt)
    if at < 0 or at + size > len(data):
        raise ValueError(f"{what}: {at:#x} with {size} bytes is outside the {len(data):#x}-byte file")
    return struct.unpack_from(fmt, data, at)


def wave_bank(data):
    """(offset, length, channels, rate, samples) per wave of a big-endian XACT 2 wave bank."""
    if data[:4] != b"DNBW":
        raise ValueError("not a big-endian wave bank")
    if len(data) < 8 + 8 * 5:
        raise ValueError("truncated wave bank header")
    segments = [be_at(data, 8 + 8 * i, ">II", "wave bank segment") for i in range(5)]
    bank = segments[0][0]
    count = be_at(data, bank + 4, ">I", "wave bank header")[0]
    size = be_at(data, bank + 0x48, ">I", "wave bank header")[0]
    if size < 16:
        raise ValueError(f"wave bank entries are {size} bytes; each is 16")
    waves = []
    for i in range(count):
        flags_duration, fmt, offset, length = be_at(data, segments[1][0] + i * size, ">4I", f"wave {i}'s entry")
        if fmt & 1 == 0:
            raise ValueError(f"wave {i} is not XMA")
        start = segments[4][0] + offset
        if start + length > len(data):
            raise ValueError(f"wave {i}'s data at {start:#x} + {length:#x} is outside the {len(data):#x}-byte file")
        waves.append((start, length, (fmt >> 1) & 7, (fmt >> 4) & 0x7FFFF, flags_duration >> 4))
    return waves


# (name, first wave, loop wave or None, whether the cue is a complex sound)
Cue = namedtuple("Cue", "name intro loop complex")


def cue_tracks(data):
    """Cue per big-endian XACT 2 sound bank, as (name, first wave, loop wave or None, complex).

    A simple sound plays one wave once. DOA4's music cues are complex sounds: their first
    track plays the intro at 0 ms, and every later track plays the loop wave again at the end
    of the one before. A complex sound with one track has no loop either, so `loop` alone
    cannot tell it from a simple sound; `complex` is what does.
    """
    if data[:4] != b"KBDS":
        raise ValueError("not a big-endian sound bank")
    if len(data) < 0x3E:
        raise ValueError("truncated sound bank header")
    cues = be_at(data, 0x09, ">h", "sound bank header")[0]
    simple = be_at(data, 0x1A, ">i", "sound bank header")[0]
    names = be_at(data, 0x3A, ">i", "sound bank header")[0]
    if cues < 0:
        raise ValueError(f"sound bank has {cues} cues")
    out = []
    for i in range(cues):
        sound = be_at(data, simple + 5 * i + 1, ">i", f"cue {i}'s entry")[0]
        name = be_at(data, names + 6 * i, ">i", f"cue {i}'s entry")[0]
        end = data.find(b"\0", name) if name >= 0 else -1
        if end < 0:
            raise ValueError(f"cue {i}'s name at {name:#x} is not in the {len(data):#x}-byte file")
        label = data[name:end].decode("ascii")
        if be_at(data, sound, ">B", f"cue {i}'s sound")[0] & 1 == 0:
            out.append(Cue(label, be_at(data, sound + 9, ">h", f"cue {i}'s sound")[0], None, False))
            continue
        waves = []
        for k in range(be_at(data, sound + 9, ">B", f"cue {i}'s sound")[0]):
            track = be_at(data, sound + 10 + 5 * k + 1, ">i", f"cue {i}'s track {k}")[0]
            # one play-wave event: dword, 2 bytes, 1 byte, then the wave
            if be_at(data, track, ">B", f"cue {i}'s track {k}")[0] >= 1:
                waves.append(be_at(data, track + 8, ">h", f"cue {i}'s track {k}")[0])
        if waves:
            loop = waves[-1] if len(waves) > 1 else None
            out.append(Cue(label, waves[0], loop, True))
    return out


def xma1_riff(raw, channels, rate):
    """A wave's raw XMA in the RIFF ffmpeg's xma1 decoder reads."""
    fmt = struct.pack("<HHHHHBB", 0x165, 16, 0x10D6, 0, 1, 0, 2)
    fmt += struct.pack("<IIIIBBH", rate * channels // 2, rate, 0, 0, 0, channels, 4 if channels == 1 else 3)
    body = b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(raw)) + raw
    return b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body
