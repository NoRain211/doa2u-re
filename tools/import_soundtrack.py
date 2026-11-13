#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Import the soundtrack of your own Dead or Alive 3 or Dead or Alive 4 for the Jukebox.

The Jukebox (recomp-runtime/jukebox.c) plays DOA2U's own music from its disc
and any album this tool imports into jukebox/<album>/ (beside mods/): one ADX
file per track and tracks.txt, which lists each track's file, loop and name.

Give it the game's disc image (.iso), the folder it was extracted to, or the
music file itself:

- Dead or Alive 3 (Xbox): bgm.afs already holds ADX, the format DOA2U plays,
  with its own loop points, so the tracks are copied as they are.
- Dead or Alive 4 (Xbox 360): SOUND/bgm_stream.xwb holds XMA. Each track is an
  intro and a loop (the cues in snd.afs's bgm sound bank say which); both are
  decoded and joined, then encoded as ADX with the loop in its header, by
  recomp_adx_encode from the game's build (the runtime's own encoder), or by
  ffmpeg before the game is built, with the loop in tracks.txt instead.
  Needs an ffmpeg with the xma1 decoder: the build fetches one into
  tools/ffmpeg on Windows; otherwise one on PATH, or given with --ffmpeg.

Each disc's movies (opening, endings, logos) are Sofdec SFD files whose sound
is ADX as well: it is taken out of them and added as tracks too. Given DOA2U's
own disc, it adds the music of DOA2U's movies as the DOA2U Movies album.

The game writes names.txt beside them ("file = name"), which players edit to
rename tracks; importing again keeps those names. Nothing this writes may be
committed.

    python tools/import_soundtrack.py "D:/Games/Dead or Alive 4.iso"
    python tools/import_soundtrack.py F:/DOA4U/extracted_ISO
    python tools/import_soundtrack.py --detect "D:/Games/Dead or Alive 3.iso"
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

from build_prerequisites import find_ffmpeg

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(ROOT, "jukebox")
SECTOR = 2048

# ---- Disc images ------------------------------------------------------------

# Where the game partition starts: a trimmed image, then Xbox (XGD1) and
# Xbox 360 (XGD2, XGD3) full images.
PARTITIONS = (0, 0x18300000, 0xFD90000, 0x2080000)
MAGIC = b"MICROSOFT*XBOX*MEDIA"


class Xdvdfs:
    """Reads files from an Xbox or Xbox 360 disc image without extracting it."""

    def __init__(self, path):
        self.file = open(path, "rb")
        self.base = None
        for base in PARTITIONS:
            self.file.seek(base + 32 * SECTOR)
            head = self.file.read(28)
            if head[:20] == MAGIC:
                self.base = base
                self.root = struct.unpack_from("<II", head, 20)
                break
        if self.base is None:
            self.file.close()
            raise ValueError(f"{path} is not an Xbox or Xbox 360 disc image")

    def close(self):
        self.file.close()

    def _read(self, sector, size):
        self.file.seek(self.base + sector * SECTOR)
        return self.file.read(size)

    def _entries(self, sector, size):
        """{lower-case name: (sector, size, is_directory, name)} of one directory."""
        data = self._read(sector, size)
        out, pending, seen = {}, [0], set()
        while pending:
            at = pending.pop()
            if at in seen or at + 14 > len(data):
                continue
            seen.add(at)
            left, right, start, length, attributes, name_length = struct.unpack_from("<HHIIBB", data, at)
            if left == 0xFFFF and right == 0xFFFF:
                continue  # padding
            name = data[at + 14:at + 14 + name_length].decode("latin-1")
            out[name.lower()] = (start, length, bool(attributes & 0x10), name)
            for child in (left, right):
                if child not in (0, 0xFFFF):
                    pending.append(child * 4)
        return out

    def find(self, path):
        """(sector, size) of a file by its path from the root, any case; None if absent."""
        sector, size = self.root
        parts = [p for p in path.replace("\\", "/").split("/") if p]
        for i, part in enumerate(parts):
            entry = self._entries(sector, size).get(part.lower())
            if entry is None or entry[2] != (i + 1 < len(parts)):
                return None
            sector, size = entry[0], entry[1]
        return sector, size

    def read(self, path):
        found = self.find(path)
        return None if found is None else self._read(*found)


class Folder:
    """The same reads from an extracted disc folder."""

    def __init__(self, path):
        self.path = path

    def close(self):
        pass

    def _resolve(self, path):
        here = self.path
        for part in [p for p in path.replace("\\", "/").split("/") if p]:
            try:
                names = {n.lower(): n for n in os.listdir(here)}
            except OSError:
                return None
            if part.lower() not in names:
                return None
            here = os.path.join(here, names[part.lower()])
        return here

    def find(self, path):
        found = self._resolve(path)
        return None if found is None or not os.path.isfile(found) else (0, os.path.getsize(found))

    def read(self, path):
        found = self._resolve(path)
        if found is None or not os.path.isfile(found):
            return None
        with open(found, "rb") as file:
            return file.read()


def open_source(path):
    if os.path.isdir(path):
        return Folder(path)
    if os.path.isfile(path) and os.path.basename(path).lower() in ("bgm.afs", "bgm_stream.xwb", "snd.afs"):
        return Folder(os.path.dirname(path))
    return Xdvdfs(path)


def detect(source):
    """doa3, doa4, doa2u or None, from the files on the disc."""
    if source.find("SOUND/bgm_stream.xwb") and source.find("snd.afs"):
        return "doa4"
    if source.find("DOA2/bgm.afs"):
        return "doa2u"
    if source.find("bgm.afs") and source.find("voice.afs"):
        return "doa3"
    return None


# ---- AFS archives -------------------------------------------------------------

def afs_table(data):
    if data[:3] != b"AFS":
        raise ValueError("not an AFS archive")
    count = struct.unpack_from("<I", data, 4)[0]
    return [struct.unpack_from("<II", data, 8 + 8 * i) for i in range(count)]


# ---- XACT 2 (Xbox 360, big-endian) --------------------------------------------

def wave_bank(data):
    """(offset, length, channels, rate, samples) per wave of a big-endian XACT 2 wave bank."""
    if data[:4] != b"DNBW":
        raise ValueError("not a big-endian wave bank")
    segments = [struct.unpack_from(">II", data, 8 + 8 * i) for i in range(5)]
    bank = segments[0][0]
    count = struct.unpack_from(">I", data, bank + 4)[0]
    size = struct.unpack_from(">I", data, bank + 0x48)[0]
    waves = []
    for i in range(count):
        flags_duration, fmt, offset, length = struct.unpack_from(">4I", data, segments[1][0] + i * size)
        if fmt & 1 == 0:
            raise ValueError(f"wave {i} is not XMA")
        waves.append((segments[4][0] + offset, length, (fmt >> 1) & 7, (fmt >> 4) & 0x7FFFF, flags_duration >> 4))
    return waves


def cue_tracks(data):
    """[(cue, intro wave, loop wave or None)] of a big-endian XACT 2 sound bank.

    A simple sound plays one wave once. DOA4's music cues are complex sounds:
    their first track plays the intro at 0 ms, and every later track plays the
    loop wave again at the end of the one before."""
    if data[:4] != b"KBDS":
        raise ValueError("not a big-endian sound bank")
    s16 = lambda at: struct.unpack_from(">h", data, at)[0]
    s32 = lambda at: struct.unpack_from(">i", data, at)[0]
    cues, simple, names = s16(0x09), s32(0x1A), s32(0x3A)
    out = []
    for i in range(cues):
        sound, name = s32(simple + 5 * i + 1), s32(names + 6 * i)
        label = data[name:data.index(b"\0", name)].decode("ascii")
        if data[sound] & 1 == 0:
            out.append((label, s16(sound + 9), None))
            continue
        waves = []
        for k in range(data[sound + 9]):
            track = s32(sound + 10 + 5 * k + 1)
            if data[track] >= 1:  # one play-wave event: dword, 2 bytes, 1 byte, then the wave
                waves.append(s16(track + 8))
        if waves:
            loop = waves[-1] if len(waves) > 1 else None
            out.append((label, waves[0], loop))
    return out


def xma1_riff(raw, channels, rate):
    fmt = struct.pack("<HHHHHBB", 0x165, 16, 0x10D6, 0, 1, 0, 2)
    fmt += struct.pack("<IIIIBBH", rate * channels // 2, rate, 0, 0, 0, channels, 4 if channels == 1 else 3)
    body = b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(raw)) + raw
    return b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body


# ---- Names --------------------------------------------------------------------

DOA4_FIGHTERS = {
    "alp": "Alpha-152", "aya": "Ayane", "bas": "Bass", "bay": "Bayman", "boy": "Eliot",
    "bra": "Brad Wong", "cri": "Christie", "ein": "Ein", "gen": "Gen Fu", "hak": "Kokoro",
    "hel": "Helena", "hyt": "Hayate", "jan": "Jann Lee", "kas": "Kasumi", "kor": "Hitomi",
    "lei": "Leifang", "leo": "Leon", "lis": "La Mariposa", "ryu": "Ryu Hayabusa",
    "spa": "Spartan-458", "tin": "Tina", "tng": "Tengu", "zac": "Zack",
}
DOA4_OTHER = {
    "select": "Character Select", "ups": "Character Select", "sparring": "Sparring",
    "shop": "Zack's Shop", "spa": "Spartan-458", "heli": "Helicopter", "market": "Market",
    "amazing": "Amazing", "eat": "Eat", "ringin": "Ringin'",
}


def doa4_name(cue):
    """A track's name from its cue: bgm_aya is Ayane, rmx_aya her remix, bgm_hel_doa2 Helena (DOA2)."""
    kind, _, rest = cue.partition("_")
    rest, _, game = rest.partition("_")
    name = DOA4_FIGHTERS.get(rest) if kind != "bgm" or rest not in DOA4_OTHER else None
    name = name or DOA4_OTHER.get(rest) or rest.replace("_", " ").title()
    if kind == "rmx":
        name += " (Remix)"
    if game:
        name += f" ({game.upper()})"
    return name


def group_name(cues):
    """One name for cues that play the same track: Kasumi / Alpha-152 (Remix); bgm_bas_doa2 is just Bass."""
    plain = [c for c in cues if c.count("_") == 1] or cues[:1]
    remix = all(c.startswith("rmx_") for c in plain)
    names = []
    for cue in plain:
        name = doa4_name(cue)
        name = name[:-len(" (Remix)")] if remix and name.endswith(" (Remix)") else name
        if name not in names:
            names.append(name)
    return " / ".join(names) + (" (Remix)" if remix else "")


# ---- Movies -------------------------------------------------------------------

def sfd_audio(data):
    """The ADX of a Sofdec movie's first audio stream (MPEG program stream 0xC0), or None."""
    out, i, n = bytearray(), 0, len(data)
    while i + 6 <= n:
        if data[i:i + 3] != b"\0\0\1":
            i = data.find(b"\0\0\1", i + 1)
            if i < 0:
                break
            continue
        sid = data[i + 3]
        if sid == 0xBA:  # pack header: MPEG-1 or MPEG-2
            i += 12 if data[i + 4] >> 4 == 2 else 14 + (data[i + 13] & 7)
            continue
        if sid == 0xB9:
            break
        if sid < 0xBB:
            i += 4
            continue
        body = i + 6
        end = body + struct.unpack_from(">H", data, i + 4)[0]
        if sid == 0xC0:
            p = body
            while p < end and data[p] == 0xFF:
                p += 1
            if p < end and data[p] & 0xC0 == 0x40:
                p += 2
            if p < end and data[p] & 0xF0 == 0x20:
                p += 5
            elif p < end and data[p] & 0xF0 == 0x30:
                p += 10
            elif p < end and data[p] == 0x0F:
                p += 1
            out += data[p:end]
        i = end
    return bytes(out) if out[:2] == b"\x80\x00" and len(out) > 64 else None


def import_movies(source, folder, movies, kept, tracks, seen=None):
    """Adds each movie's music as m_<movie>.adx: movies [(path on the disc, name)]. Skips repeats."""
    seen = set() if seen is None else seen
    for path, label in movies:
        data = source.read(path)
        audio = sfd_audio(data) if data else None
        if audio is None or audio in seen:
            continue
        seen.add(audio)
        name = "m_" + os.path.splitext(os.path.basename(path))[0].lower() + ".adx"
        with open(os.path.join(folder, name), "wb") as file:
            file.write(audio)
        tracks.append((name, -2, -2, kept.get(name, label)))
        print(f"  {name}: {tracks[-1][3]}", flush=True)
    return seen


DOA3_CODES = {
    "ay": "Ayane", "ba": "Bass", "fa": "Gen Fu", "go": "Christie", "gy": "Leon", "hi": "Helena",
    "kl": "Zack", "km": "Ryu Hayabusa", "ko": "Hitomi", "ks": "Kasumi", "le": "Leifang", "lu": "Jann Lee",
    "ni": "Hayate", "sa": "Bayman", "su": "Brad Wong", "tn": "Tina",
}
DOA3_MOVIES = [("mv_op.sfd", "Opening Movie"), ("mv_st.sfd", "Staff Roll Movie")] + \
    [(f"mv_{code}.sfd", f"{name}'s Ending") for code, name in DOA3_CODES.items()] + [("ninja.sfd", "Team Ninja Logo")]
DOA4_MOVIES = [(f"movie/{stem}.sfd", f"{name}'s Ending") for stem, name in (
    ("ayane", "Ayane"), ("bass", "Bass"), ("bayman", "Bayman"), ("brad", "Brad Wong"), ("christie", "Christie"),
    ("eliot", "Eliot"), ("hayabusa", "Ryu Hayabusa"), ("hayate", "Hayate"), ("helena", "Helena"),
    ("hitomi", "Hitomi"), ("jannlee", "Jann Lee"), ("kasumi", "Kasumi"), ("kokoro", "Kokoro"),
    ("leifang", "Leifang"), ("lisa", "La Mariposa"), ("tina", "Tina"), ("zack", "Zack"))] + \
    [("movie/doa4_usa_cm.sfd", "TV Commercial"), ("movie/ninja_vi_hd.sfd", "Team Ninja Logo")]
# DOA1's movies (DOA1/ss_opn.sfd, ss_end.sfd) are DOA1/Sound's opmov_sng and edmov_sng, which the game reads.
DOA2U_MOVIES = [("DOA2/doa2_op.sfd", "Opening Movie"), ("DOA2/DOA_G4.sfd", "G4 Movie"),
    ("DOA2/2_VI_WIDE_B.sfd", "Team Ninja Logo"), ("DOA2/4_VI_STD_B.sfd", "Team Ninja Logo")]


# Dead or Alive Ultimate's DOA1 music on DOA2U's disc (DOA1/Sound), in Doa1.xbe's music table order
# (0x2d50a8); the codes are DOA3's, and gy is the slot Leon took in DOA2: Bayman's in DOA1.
DOA1U_NAMES = [
    ("ks_sng", "Kasumi"), ("lu_sng", "Jann Lee"), ("le_sng", "Leifang"), ("km_sng", "Ryu Hayabusa"),
    ("gy_sng", "Bayman"), ("fa_sng", "Gen Fu"), ("kl_sng", "Zack"), ("tn_sng", "Tina"), ("rd_sng", "Raidou"),
    ("sel_sng", "Character Select"), ("name_sng", "Name Entry"), ("cnt_sng", "Continue"), ("clr_sng", "Clear"),
    ("trng_sng", "Training"), ("kl_lose", "Zack (Lose)"), ("le_lose", "Leifang (Lose)"),
    ("km_lose", "Ryu Hayabusa (Lose)"), ("lu_lose", "Jann Lee (Lose)"), ("tn_lose", "Tina (Lose)"),
    ("gy_lose", "Bayman (Lose)"), ("fa_lose", "Gen Fu (Lose)"), ("ks_lose", "Kasumi (Lose)"),
    ("ks_lose2", "Kasumi (Lose 2)"), ("rd_lose", "Raidou (Lose)"), ("opmov_sng", "Opening Movie"),
    ("edmov_sng", "Ending Movie"), ("online_sng", "Online"),
]


def import_doa1u(source, out, encoder=None):
    """DOA2U's disc's DOA1 music, copied (and levelled) as the DOA1U album."""
    folder = os.path.join(out, "doa1u")
    os.makedirs(folder, exist_ok=True)
    kept = read_names(folder)
    tracks = []
    for stem, label in DOA1U_NAMES:
        data = source.read(f"DOA1/Sound/{stem}.adx")
        if not data or data[:2] != b"\x80\x00":
            continue
        name = f"{stem}.adx"
        with open(os.path.join(folder, name), "wb") as file:
            file.write(data)
        tracks.append((name, -1, -1, kept.get(name, label)))
    write_tracks(folder, "Dead or Alive Ultimate", tracks)
    print(f"Dead or Alive Ultimate (DOA1): {len(tracks)} tracks in {folder}")
    level_album(folder, encoder)
    return len(tracks)


def import_doa2u(source, out, encoder=None):
    """DOA2U's own disc: its DOA1 music as the DOA1U album, and its movies' music as the DOA2U
    Movies album (its bgm.afs the game reads itself)."""
    import_doa1u(source, out, encoder)
    folder = os.path.join(out, "doa2u-movies")
    os.makedirs(folder, exist_ok=True)
    kept = read_names(folder)
    tracks = []
    import_movies(source, folder, DOA2U_MOVIES, kept, tracks)
    write_tracks(folder, "Dead or Alive 2 Ultimate (Movies)", tracks)
    level_album(folder, encoder)
    print(f"Dead or Alive 2 Ultimate movies: {len(tracks)} tracks in {folder}")
    return len(tracks)


# ---- Output -------------------------------------------------------------------

def read_names(folder):
    """{file: name} from an earlier import's tracks.txt, so renamed tracks keep their names."""
    names = {}
    try:
        with open(os.path.join(folder, "tracks.txt"), encoding="utf-8") as file:
            for line in file:
                parts = line.rstrip("\r\n").split("\t")
                if len(parts) >= 4 and not line.startswith("#"):
                    names[parts[0]] = parts[3]
    except OSError:
        pass
    return names


def write_tracks(folder, title, tracks):
    """tracks: [(file, loop start, loop end, name)] in samples. -1 loops at the ADX header's own
    points, or not at all if it has none; -2 never loops."""
    with open(os.path.join(folder, "tracks.txt"), "w", encoding="utf-8", newline="\n") as file:
        file.write(f"#title\t{title}\n")
        file.write("# file<TAB>loop start sample<TAB>loop end sample<TAB>name. -1: the file's own loop; -2: no loop.\n")
        for name, start, end, label in tracks:
            file.write(f"{name}\t{start}\t{end}\t{label}\n")


# DOA2U's own music plays at about -17 dBFS (RMS); DOA3's at about -23.5 and DOA4's at about -12.
LEVEL_DB = -17.0


def level_album(folder, encoder):
    """Brings each track of an album to LEVEL_DB with recomp_adx_encode --level, once: tracks.txt
    then says #level. Without the encoder (build the game first) it leaves them as they are."""
    path = os.path.join(folder, "tracks.txt")
    try:
        with open(path, encoding="utf-8") as file:
            lines = file.read().splitlines()
    except OSError:
        return 0
    mark = f"#level\t{LEVEL_DB}"
    if mark in lines:
        return 0
    if not encoder:
        print(f"{os.path.basename(folder)}: build the game first to level its loudness with DOA2U's music.")
        return 0
    count = 0
    for line in lines:
        parts = line.split("\t")
        if line.startswith("#") or len(parts) < 4:
            continue
        track = os.path.join(folder, parts[0])
        scratch = track + ".level"
        result = subprocess.run([encoder, "--level", str(LEVEL_DB), track, scratch], capture_output=True, text=True)
        if result.returncode != 0:
            print(f"  {parts[0]}: not levelled ({result.stderr.strip()})")
            if os.path.exists(scratch):
                os.remove(scratch)
            continue
        os.replace(scratch, track)
        count += 1
        print(f"  {parts[0]}: {result.stdout.strip()}", flush=True)
    at = 1 if lines and lines[0].startswith("#title") else 0
    lines.insert(at, mark)
    with open(path, "w", encoding="utf-8", newline="\n") as file:
        file.write("\n".join(lines) + "\n")
    return count


# bgm.afs's tracks in its order: the game's music table (0x2d4918 in the Japanese
# Default.xbe) names them, and the character table at 0x3034e0 says whose theme
# each character's is.
DOA3_NAMES = [
    "Kasumi", "Leon", "Gen Fu", "Zack", "Omega", "Leifang", "Jann Lee", "Tina", "Bass",
    "Ryu Hayabusa", "Ayane", "Helena", "Hayate", "Brad Wong", "Hitomi", "Christie", "Bayman",
    "Character Select", "Name Entry", "Training", "Tokyo Game Show Demo", "Short Jingle",
    "Amazing (Aerosmith)", "Home Tonight (Aerosmith)", "Nine Lives (Aerosmith)", "Staff Roll",
]


def import_doa3(source, out, encoder=None):
    data = source.read("bgm.afs")
    folder = os.path.join(out, "doa3")
    os.makedirs(folder, exist_ok=True)
    kept = read_names(folder)
    tracks = []
    for i, (offset, length) in enumerate(afs_table(data)):
        track = data[offset:offset + length]
        if length < 64 or track[:2] != b"\x80\x00":
            continue  # padding entries
        name = f"{i + 1:02d}.adx"
        with open(os.path.join(folder, name), "wb") as file:
            file.write(track)
        default = DOA3_NAMES[i] if i < len(DOA3_NAMES) else f"Track {i + 1:02d}"
        label = kept.get(name, default)
        if label == f"Track {i + 1:02d}":  # an earlier import's placeholder
            label = default
        tracks.append((name, -1, -1, label))
    import_movies(source, folder, DOA3_MOVIES, kept, tracks)
    write_tracks(folder, "Dead or Alive 3", tracks)
    level_album(folder, encoder)
    print(f"Dead or Alive 3: {len(tracks)} tracks in {folder}")
    return len(tracks)


# ffmpeg's XMA decoder starts every wave 448 samples late and pads its end to
# a whole 512-sample frame; the wave bank gives each wave's own length.
XMA_DELAY = 448


def decode(ffmpeg, raw, channels, rate, samples, target):
    """XMA to 16-bit stereo PCM at the wave's own rate, exactly the wave's samples long."""
    with tempfile.TemporaryDirectory() as scratch:
        source = os.path.join(scratch, "wave.wav")
        with open(source, "wb") as file:
            file.write(xma1_riff(raw, channels, rate))
        subprocess.run([ffmpeg, "-v", "error", "-y", "-i", source, "-f", "s16le", "-ac", "2", "-ar", str(rate),
            target], check=True)
    with open(target, "rb") as file:
        pcm = file.read()
    pcm = pcm[XMA_DELAY * 4:(XMA_DELAY + samples) * 4]
    return pcm + bytes(samples * 4 - len(pcm))


def write_wav(path, pcm, rate):
    """16-bit stereo PCM as a WAV."""
    fmt = struct.pack("<HHIIHH", 1, 2, rate, rate * 4, 4, 16)
    with open(path, "wb") as file:
        file.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack("<I", 16) + fmt +
            b"data" + struct.pack("<I", len(pcm)))
        file.write(pcm)


def find_encoder(given):
    """recomp_adx_encode from the game's build (Launcher.exe builds it), or None."""
    if given:
        return given
    for folder in ("build/recomp-program-mingw", "build/recomp-program"):
        for name in ("recomp_adx_encode.exe", "recomp_adx_encode"):
            path = os.path.join(ROOT, folder, name)
            if os.path.isfile(path):
                return path
    return None


def import_doa4(source, out, ffmpeg, encoder=None):
    bank_data = source.read("SOUND/bgm_stream.xwb")
    afs = source.read("snd.afs")
    table = afs_table(afs)
    sounds = None
    for offset, length in table:
        entry = afs[offset:offset + length]
        if entry[:4] == b"KBDS" and b"bgm_stream" in entry[:0x100]:
            sounds = entry
            break
    if sounds is None:
        raise SystemExit("snd.afs has no bgm_stream sound bank; is this Dead or Alive 4?")
    waves = wave_bank(bank_data)
    folder = os.path.join(out, "doa4")
    os.makedirs(folder, exist_ok=True)
    kept = read_names(folder)
    groups = {}  # (intro, loop) -> its cues, in bank order: rmx_kas and rmx_alp are one remix
    for cue, intro, loop in cue_tracks(sounds):
        if all(w is None or w < len(waves) for w in (intro, loop)):
            groups.setdefault((intro, loop), []).append(cue)
    # Themes first by name, then the rest, then the remixes.
    order = sorted(groups.items(), key=lambda g: (g[1][0].startswith("rmx_"),
        g[1][0].split("_")[1] not in DOA4_FIGHTERS, group_name(g[1])))
    tracks = []
    if not encoder:
        print("recomp_adx_encode is not built yet (build the game first); encoding with ffmpeg instead.")
    with tempfile.TemporaryDirectory() as scratch:
        for (intro, loop), cues in order:
            parts = [w for w in (intro, loop) if w is not None]
            pcm, rate = [], waves[parts[0]][3]
            for w in parts:
                offset, length, channels, _, samples = waves[w]
                pcm.append(decode(ffmpeg, bank_data[offset:offset + length], channels, rate, samples,
                    os.path.join(scratch, f"{w}.pcm")))
            name = f"{len(tracks) + 1:02d}_{cues[0]}.adx"
            frames = [len(p) // 4 for p in pcm]
            start, end = (frames[0], frames[0] + frames[1]) if loop is not None else (-2, -2)
            target = os.path.join(folder, name)
            if encoder:
                # The runtime's own encoder writes the loop into the ADX header.
                joined = os.path.join(scratch, "track.wav")
                write_wav(joined, b"".join(pcm), rate)
                subprocess.run([encoder, joined, target] + ([str(start), str(end)] if loop is not None else []),
                    check=True)
                if loop is not None:
                    start = end = -1
            else:
                joined = os.path.join(scratch, "track.pcm")
                with open(joined, "wb") as file:
                    file.write(b"".join(pcm))
                subprocess.run([ffmpeg, "-v", "error", "-y", "-f", "s16le", "-ac", "2", "-ar", str(rate), "-i", joined,
                    "-c:a", "adpcm_adx", "-f", "adx", target], check=True, stderr=subprocess.DEVNULL)
            tracks.append((name, start, end, kept.get(name, group_name(cues))))
            print(f"  {name}: {tracks[-1][3]}", flush=True)
    import_movies(source, folder, DOA4_MOVIES, kept, tracks)
    write_tracks(folder, "Dead or Alive 4", tracks)
    level_album(folder, encoder)
    print(f"Dead or Alive 4: {len(tracks)} tracks in {folder}")
    return len(tracks)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", nargs="?", help="the disc image, its extracted folder, or bgm.afs / snd.afs")
    parser.add_argument("--out", default=DEFAULT_OUT, help="the Jukebox's albums (default jukebox)")
    parser.add_argument("--ffmpeg", help="ffmpeg with the xma1 decoder (default: the build's tools/ffmpeg, else PATH)")
    parser.add_argument("--adx-encoder", help="recomp_adx_encode (default: the one the game's build made)")
    parser.add_argument("--detect", action="store_true", help="print doa3, doa4 or doa2u and stop")
    parser.add_argument("--level", action="store_true",
                        help="level the loudness of every album already imported (no disc needed)")
    args = parser.parse_args()
    if args.level:
        encoder = find_encoder(args.adx_encoder)
        for name in sorted(os.listdir(args.out)) if os.path.isdir(args.out) else []:
            level_album(os.path.join(args.out, name), encoder)
        return 0
    if not args.source:
        parser.error("give a disc, or --level")
    try:
        source = open_source(args.source)
    except (OSError, ValueError) as error:
        if args.detect:
            print("unknown")
            return 0
        raise SystemExit(str(error))
    try:
        game = detect(source)
        if args.detect:
            print(game or "unknown")
            return 0
        if game == "doa3":
            import_doa3(source, args.out, find_encoder(args.adx_encoder))
        elif game == "doa4":
            import_doa4(source, args.out, args.ffmpeg or find_ffmpeg(ROOT), find_encoder(args.adx_encoder))
        elif game == "doa2u":
            import_doa2u(source, args.out, find_encoder(args.adx_encoder))
        else:
            raise SystemExit("Not a Dead or Alive 3 or Dead or Alive 4 disc.")
    except FileNotFoundError:
        raise SystemExit("ffmpeg was not found. Install one with the xma1 decoder, or pass --ffmpeg.")
    finally:
        source.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
