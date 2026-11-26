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
  Needs an ffmpeg with the xma1 decoder: on Windows this fetches one into
  tools/ffmpeg on first use when none is bundled or on PATH; otherwise give
  one with --ffmpeg.

Each disc's movies (opening, endings, logos) are Sofdec SFD files whose sound
is ADX as well: every SFD in its movie folders is taken out of it and added as
a track too. Given DOA2U's own disc, it adds the music of DOA2U's movies as the
DOA2U Movies album, and its DOA1 music (DOA1/Sound, listed by Doa1.xbe) as the
DOA1U album.

The game writes names.txt beside them ("file = name"), which players edit to
rename tracks; importing again keeps those names. Nothing this writes may be
committed.

    python tools/import_soundtrack.py "D:/Games/Dead or Alive 4.iso"
    python tools/import_soundtrack.py F:/DOA4U/extracted_ISO
    python tools/import_soundtrack.py --detect "D:/Games/Dead or Alive 3.iso"
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

from build_prerequisites import ffmpeg as fetch_ffmpeg, find_ffmpeg
import xbe_info
from xact import afs_table, cue_tracks, wave_bank, xma1_riff

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(ROOT, "jukebox")
SECTOR = 2048


def run_tool(argv, **kwargs):
    """subprocess.run that names the program it could not start: Windows leaves the name out."""
    try:
        return subprocess.run(argv, **kwargs)
    except FileNotFoundError as error:
        raise FileNotFoundError(error.errno, error.strerror, argv[0]) from None


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

    def list(self, path):
        """A folder's file names, in any case; empty if it is not there."""
        sector, size = self.root
        for part in [p for p in path.replace("\\", "/").split("/") if p]:
            entry = self._entries(sector, size).get(part.lower())
            if entry is None or not entry[2]:
                return []
            sector, size = entry[0], entry[1]
        return sorted((name for _start, _length, is_dir, name in self._entries(sector, size).values()
                       if not is_dir), key=str.lower)


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

    def list(self, path):
        """A folder's file names; empty if it is not there."""
        here = self._resolve(path)
        if here is None or not os.path.isdir(here):
            return []
        return sorted((name for name in os.listdir(here)
                       if os.path.isfile(os.path.join(here, name))), key=str.lower)


def disc_root(folder):
    """The folder the game's paths start from. A DOA2U extraction's DOA2 (or DOA1) folder is one
    below it, and the bgm.afs in it is DOA2U's own music, not DOA3's."""
    folder = os.path.abspath(folder)
    if os.path.basename(folder).lower() in ("doa1", "doa2"):
        return os.path.dirname(folder)
    return folder


def open_source(path):
    if os.path.isdir(path):
        return Folder(disc_root(path))
    if os.path.isfile(path) and os.path.basename(path).lower() in ("bgm.afs", "bgm_stream.xwb", "snd.afs"):
        return Folder(disc_root(os.path.dirname(os.path.abspath(path))))
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
# The fighter codes the games' tables, cues and file names use. DOA1 shares them, with Raidou,
# and gy is the slot Leon took in DOA2: Bayman's in DOA1.
DOA3_CODES = {
    "ay": "Ayane", "ba": "Bass", "fa": "Gen Fu", "go": "Christie", "gy": "Leon", "hi": "Helena",
    "kl": "Zack", "km": "Ryu Hayabusa", "ko": "Hitomi", "ks": "Kasumi", "le": "Leifang", "lu": "Jann Lee",
    "ni": "Hayate", "sa": "Bayman", "su": "Brad Wong", "tn": "Tina",
}
DOA1_CODES = {**{code: DOA3_CODES[code] for code in ("ks", "lu", "le", "km", "fa", "kl", "tn")},
              "gy": "Bayman", "rd": "Raidou"}
DOA1_OTHER = {
    "sel": "Character Select", "name": "Name Entry", "cnt": "Continue", "clr": "Clear",
    "trng": "Training", "opmov": "Opening Movie", "edmov": "Ending Movie", "online": "Online",
}
DOA1_KINDS = {"sng": "", "lose": " (Lose)", "lose2": " (Lose 2)"}


def words_label(stem):
    """A name from a file's own words: its underscores and hyphens become spaces."""
    return " ".join(word.capitalize() for word in re.split(r"[_-]+", stem) if word)


def doa1u_name(file):
    """A track's name from its file: ks_sng is Kasumi's theme, ks_lose the one she loses to."""
    stem = os.path.splitext(os.path.basename(file))[0]
    code, _, kind = stem.partition("_")
    name = DOA1_CODES.get(code) or DOA1_OTHER.get(code)
    if name is None or (kind and kind not in DOA1_KINDS):
        return words_label(stem)
    return name + DOA1_KINDS.get(kind, "")


def movie_name(file):
    """A movie's name from its file: mv_<code> is that fighter's ending, else its own words."""
    stem = os.path.splitext(os.path.basename(file))[0]
    kind, _, code = stem.partition("_")
    if kind.lower() == "mv" and code in DOA3_CODES:
        return f"{DOA3_CODES[code]}'s Ending"
    return words_label(stem)


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


def import_movies(source, folder, where, kept, tracks, seen=None):
    """Adds the music of each *.sfd movie in a folder on the disc as m_<movie>.adx. Skips repeats."""
    seen = set() if seen is None else seen
    for file in source.list(where):
        if not file.lower().endswith(".sfd"):
            continue
        data = source.read(f"{where}/{file}" if where else file)
        audio = sfd_audio(data) if data else None
        if audio is None or audio in seen:
            continue
        seen.add(audio)
        # Every name here goes into tracks.txt, which only takes [A-Za-z0-9_.-]: a movie file
        # named after the disc's own title must not abort the album after it was written.
        # Two movie names can sanitise to one file ("a b" and "a_b" both give m_a_b.adx), so
        # the later one takes a number rather than overwriting the earlier movie's music.
        base = "m_" + safe_cue(os.path.splitext(os.path.basename(file))[0].lower())
        name, repeat = base + ".adx", 2
        while any(track[0] == name for track in tracks):
            name, repeat = f"{base}_{repeat}.adx", repeat + 1
        with open(os.path.join(folder, name), "wb") as handle:
            handle.write(audio)
        tracks.append((name, -2, -2, kept.get(name, movie_name(file))))
        print(f"  {name}: {tracks[-1][3]}", flush=True)
    return seen


# Doa1.xbe's music table: a pointer per file in DOA1/Sound, in the game's order, ended by 0.
DOA1_TABLE = 0x2E02F8


def xbe_music(data, table):
    """The file names an XBE's music table points at, in its order."""
    base, sections = xbe_info.load(data)
    files = []
    for i in range(256):
        pointer, = struct.unpack("<I", xbe_info.read(data, sections, table + 4 * i, 4))
        if not pointer:
            break
        files.append(xbe_info.text(data, sections, pointer))
    return files


def import_doa1u(source, out, encoder=None):
    """DOA2U's disc's DOA1 music, copied (and levelled) as the DOA1U album."""
    executable = source.read("Doa1.xbe")
    try:
        files = xbe_music(executable, DOA1_TABLE) if executable else []
    except (ValueError, struct.error):
        files = []
    if not files:
        print("Doa1.xbe's music list was not found; the DOA1 album is left as it is.")
        return 0
    folder = os.path.join(out, "doa1u")
    os.makedirs(folder, exist_ok=True)
    kept = read_names(folder)
    tracks = []
    for name in files:
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", name) or name in (".", ".."):
            continue
        data = source.read(f"DOA1/Sound/{name}")
        if not data or data[:2] != b"\x80\x00":
            continue
        with open(os.path.join(folder, name), "wb") as file:
            file.write(data)
        tracks.append((name, -1, -1, kept.get(name, doa1u_name(name))))
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
    import_movies(source, folder, "DOA2", kept, tracks)
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


def atomic_text(path, text):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n",
                                         dir=os.path.dirname(path), delete=False) as file:
            temporary = file.name
            file.write(text)
            file.flush()
            os.fsync(file.fileno())
        os.replace(temporary, path)
    finally:
        if temporary and os.path.exists(temporary):
            os.remove(temporary)


def record_text(text):
    return re.sub(r"[\t\r\n]", " ", text)


def safe_cue(cue):
    return re.sub(r"[^A-Za-z0-9_-]", "_", cue)[:48] or "track"


def track_name(name):
    """A track's file name as tracks.txt may say it: a bare name inside its album folder."""
    if name in (".", "..") or not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
        raise ValueError(f"invalid track file name: {name!r}")
    return name


def write_tracks(folder, title, tracks):
    """tracks: [(file, loop start, loop end, name)] in samples. -1 loops at the ADX header's own
    points, or not at all if it has none; -2 never loops."""
    lines = [f"#title\t{record_text(title)}",
             "# file<TAB>loop start sample<TAB>loop end sample<TAB>name. -1: the file's own loop; -2: no loop."]
    for name, start, end, label in tracks:
        lines.append(f"{track_name(name)}\t{start}\t{end}\t{record_text(label)}")
    atomic_text(os.path.join(folder, "tracks.txt"), "\n".join(lines) + "\n")


# DOA2U's own music plays at about -17 dBFS (RMS); DOA3's at about -23.5 and DOA4's at about -12.
LEVEL_DB = -17.0


def swap_levelled(folder, scratch, track):
    """Puts a levelled scratch file in its track's place. One that cannot be keeps its scratch file
    for the next import to try again, and the rest go on. The scratch file sits beside its track,
    so this fails only on the folder or the track: open elsewhere (gone next time) or read-only
    (said again on every import until it is fixed)."""
    try:
        os.replace(scratch, track)
    except OSError as error:
        print(f"warning: {os.path.basename(folder)}: {os.path.basename(track)} is still unlevelled: the "
              f"levelled copy could not replace it ({error}). {os.path.basename(scratch)} stays beside it, "
              "and every import tries the swap again until it works.")


def track_loop(parts):
    """A row's loop fields as (start, end) samples, or None when they are not sample numbers."""
    if len(parts) < 4 or not re.fullmatch(r"-?\d+", parts[1]) or not re.fullmatch(r"-?\d+", parts[2]):
        return None
    return int(parts[1]), int(parts[2])


def has_explicit_loop(line):
    """Whether a tracks.txt row names the loop itself rather than the file's header."""
    loop = track_loop(line.split("\t"))
    return loop is not None and loop[0] >= 0


def album_track(folder, name):
    """The track a tracks.txt row names, or None when its name is not one (left alone, said once)."""
    try:
        return os.path.join(folder, track_name(name))
    except ValueError:
        print(f"warning: {os.path.basename(folder)}: {name!r} is not a track file name; "
            "its row is left as it is.")
        return None


def finish_swaps(folder, lines):
    """Finishes a swap a failed replace cut short: a levelled copy left beside its track."""
    for line in lines:
        parts = line.split("\t")
        if not line.startswith("#") and len(parts) >= 4:
            track = album_track(folder, parts[0])
            if track is not None and os.path.exists(track + ".level"):
                swap_levelled(folder, track + ".level", track)


def level_album(folder, encoder):
    """Brings each track of an album to LEVEL_DB with recomp_adx_encode --level, once: tracks.txt
    then says #level. Without the encoder (build the game first) it leaves them as they are.
    Every track is levelled into a scratch file beside it first, and only a run where they all
    succeeded replaces the tracks: a failure leaves the album exactly as it was, so a rerun levels
    the same input rather than levelling the tracks that worked a second time. Once they all
    succeeded, the #level mark is written first and the scratch files then replace the tracks: if
    a replace fails (a track open elsewhere), that track keeps its scratch file and a rerun
    swaps it in rather than levelling again. Returns how many tracks it levelled successfully.

    A row's own loop points go to the encoder too, and the row then reads -1 -1: the header holds
    that loop, frame-aligned. The game loops an assigned track from its ADX header, and a DOA4
    album ffmpeg encoded before the game was built has its loop only in tracks.txt, so levelling
    with that row is what puts the loop in the header and keeps long matches playing. The mark
    says loop for the same reason; an album the earlier mark levelled without one is levelled
    again only when a row still holds a loop to restore."""
    path = os.path.join(folder, "tracks.txt")
    try:
        with open(path, encoding="utf-8") as file:
            lines = file.read().splitlines()
    except OSError:
        return 0
    mark = f"#level\t{LEVEL_DB}\tloop"
    legacy = f"#level\t{LEVEL_DB}"
    if mark in lines:
        finish_swaps(folder, lines)
        return 0
    if legacy in lines and not any(has_explicit_loop(line) for line in lines if not line.startswith("#")):
        # Levelled before loops were kept, and no row holds a loop its file could have lost:
        # the new mark alone says so, without another lossy pass over every track.
        finish_swaps(folder, lines)
        lines[lines.index(legacy)] = mark
        atomic_text(path, "\n".join(lines) + "\n")
        return 0
    if not encoder:
        print(f"{os.path.basename(folder)}: build the game first to level its loudness with DOA2U's music.")
        return 0
    levelled, failed, skipped, committed = [], False, False, False
    try:
        for index, line in enumerate(lines):
            parts = line.split("\t")
            if line.startswith("#") or len(parts) < 4:
                continue
            track = album_track(folder, parts[0])
            if track is None:
                skipped = True
                continue
            scratch = track + ".level"
            loop = track_loop(parts)
            if loop is None:
                print(f"  {parts[0]}: its loop points are not samples ({parts[1]!r} {parts[2]!r}); "
                    "levelling with the loop in the file's header instead.")
            # -1 says the header has the loop, -2 no loop, and samples given win.
            args = [] if loop in (None, (-1, -1)) else [parts[1], parts[2]]
            result = run_tool([encoder, "--level", str(LEVEL_DB), *args, track, scratch],
                capture_output=True, text=True)
            if result.returncode != 0:
                failed = True
                print(f"  {parts[0]}: not levelled ({result.stderr.strip()})")
                if os.path.exists(scratch):
                    os.remove(scratch)
                continue
            if loop is not None and loop[0] >= 0 and loop[1] > loop[0]:
                lines[index] = "\t".join([parts[0], "-1", "-1", *parts[3:]])
            levelled.append((scratch, track))
            print(f"  {parts[0]}: {result.stdout.strip()}", flush=True)
        if failed:
            print(f"{os.path.basename(folder)}: not every track was levelled; the album is as it was.")
            return len(levelled)
        if skipped and not levelled:
            # Nothing was levelled, so no mark: fix the names and the next import tries again.
            print(f"{os.path.basename(folder)}: no track was levelled; the album is as it was.")
            return 0
        lines = [line for line in lines if line != legacy]
        at = 1 if lines and lines[0].startswith("#title") else 0
        lines.insert(at, mark)
        atomic_text(path, "\n".join(lines) + "\n")
        committed = True  # from here a scratch file left behind is levelled audio a rerun swaps in
        for scratch, track in levelled:
            swap_levelled(folder, scratch, track)
        return len(levelled)
    finally:
        for scratch, _track in levelled:
            if not committed and os.path.exists(scratch):
                os.remove(scratch)


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
    import_movies(source, folder, "", kept, tracks)
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
        run_tool([ffmpeg, "-v", "error", "-y", "-i", source, "-f", "s16le", "-ac", "2", "-ar", str(rate),
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


def resolve_ffmpeg(given=None):
    """The ffmpeg with the xma1 decoder DOA4 needs, fetching it (Windows) when none is found."""
    tool = given or find_ffmpeg(ROOT) or fetch_ffmpeg(ROOT)
    try:
        check = run_tool([tool, "-hide_banner", "-decoders"], check=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0)
    except subprocess.CalledProcessError as error:  # a broken ffmpeg is reported, not a traceback
        raise ValueError(f"{tool} could not list its decoders (exit {error.returncode})") from error
    except FileNotFoundError:
        raise  # main names the missing ffmpeg itself
    except OSError as error:  # not executable, or not a program for this machine
        raise ValueError(f"{tool} could not be run: {error.strerror or error}") from error
    if not re.search(r"\bxma1\b", check.stdout):
        raise ValueError("FFmpeg lacks the XMA decoder required by DOA4")
    return tool


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
    for cue, intro, loop, _complex in cue_tracks(sounds):
        if any(w is not None for w in (intro, loop)) and all(w is None or 0 <= w < len(waves) for w in (intro, loop)):
            groups.setdefault((intro, loop), []).append(cue)
    # Themes first by name, then the rest, then the remixes.
    order = sorted(groups.items(), key=lambda g: (g[1][0].startswith("rmx_"),
        (g[1][0].split("_") + [""])[1] not in DOA4_FIGHTERS, group_name(g[1])))
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
            name = f"{len(tracks) + 1:02d}_{safe_cue(cues[0])}.adx"
            frames = [len(p) // 4 for p in pcm]
            start, end = (frames[0], frames[0] + frames[1]) if loop is not None else (-2, -2)
            target = os.path.join(folder, name)
            if encoder:
                # The runtime's own encoder writes the loop into the ADX header.
                joined = os.path.join(scratch, "track.wav")
                write_wav(joined, b"".join(pcm), rate)
                run_tool([encoder, joined, target] + ([str(start), str(end)] if loop is not None else []),
                    check=True)
                if loop is not None:
                    start = end = -1
            else:
                joined = os.path.join(scratch, "track.pcm")
                with open(joined, "wb") as file:
                    file.write(b"".join(pcm))
                run_tool([ffmpeg, "-v", "error", "-y", "-f", "s16le", "-ac", "2", "-ar", str(rate), "-i", joined,
                    "-c:a", "adpcm_adx", "-f", "adx", target], check=True, stderr=subprocess.DEVNULL)
            tracks.append((name, start, end, kept.get(name, group_name(cues))))
            print(f"  {name}: {tracks[-1][3]}", flush=True)
    import_movies(source, folder, "movie", kept, tracks)
    write_tracks(folder, "Dead or Alive 4", tracks)
    level_album(folder, encoder)
    print(f"Dead or Alive 4: {len(tracks)} tracks in {folder}")
    return len(tracks)


FFMPEG_MISSING = "ffmpeg was not found. Install one with the xma1 decoder, or pass --ffmpeg."


def missing_message(error, ffmpeg):
    """A FileNotFoundError's message: ffmpeg only when ffmpeg is what is missing, else its path."""
    path = error.filename or ""
    if not path:
        return str(error)
    if ffmpeg and os.path.normcase(os.path.abspath(path)) == os.path.normcase(os.path.abspath(ffmpeg)):
        return FFMPEG_MISSING
    return f"{path} was not found."


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
        try:
            for name in sorted(os.listdir(args.out)) if os.path.isdir(args.out) else []:
                level_album(os.path.join(args.out, name), encoder)
        except FileNotFoundError as error:
            raise SystemExit(missing_message(error, None))
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
    ffmpeg = None
    try:
        game = detect(source)
        if args.detect:
            print(game or "unknown")
            return 0
        if game == "doa3":
            import_doa3(source, args.out, find_encoder(args.adx_encoder))
        elif game == "doa4":
            ffmpeg = resolve_ffmpeg(args.ffmpeg)
            import_doa4(source, args.out, ffmpeg, find_encoder(args.adx_encoder))
        elif game == "doa2u":
            import_doa2u(source, args.out, find_encoder(args.adx_encoder))
        else:
            raise SystemExit("Not a Dead or Alive 3 or Dead or Alive 4 disc.")
    except FileNotFoundError as error:
        raise SystemExit(missing_message(error, ffmpeg))
    except ValueError as error:  # a corrupt archive or bank is the disc's problem, not a crash
        raise SystemExit(str(error))
    finally:
        source.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
