# SPDX-License-Identifier: GPL-3.0-or-later
"""Check GitHub for a newer release ZIP of this platform and unpack it over the install.

The launchers run this: "check" prints the newer version, if any; "install" unpacks it.
Everything it prints goes to stdout, and it exits 1 on failure with the reason as the last line.
The release ZIP's VERSION file says what is installed; a source checkout has none and is never updated.
"""
import hashlib
import json
from pathlib import Path
import re
import sys
import tempfile
import urllib.request
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
LATEST = "https://api.github.com/repos/NoRain211/doa2u-re/releases/latest"
OTHER_PLATFORMS = ("-macOS.zip", "-Linux.zip")
SUFFIX = {"win32": "", "darwin": "-macOS"}.get(sys.platform, "-Linux")


def number(text):
    found = re.search(r"\d+(\.\d+)+", text)
    return tuple(int(part) for part in found.group().split(".")) if found else (0,)


def installed():
    version = ROOT / "VERSION"
    return version.read_text(encoding="utf-8").strip() if version.is_file() else None


def fetch(url, start=None, end=None):
    """Open url, or only bytes start..end-1 of it."""
    try:
        import truststore  # the Windows release bundles it so Windows verifies certificates; elsewhere Python's CA store is used
        truststore.inject_into_ssl()
    except ImportError:
        pass
    headers = {"User-Agent": "DOA2U-launcher"}
    if start is not None:
        headers["Range"] = f"bytes={start}-{end - 1}"  # urllib keeps it across GitHub's redirect to its CDN
    return urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=30)


class Ranges:
    """A read-only, seekable view of a remote file that downloads only the byte ranges read from it.

    zipfile reads the central directory and then just the entries asked for, so an update fetches only those."""
    BLOCK = 1 << 20  # read ahead, so the many small reads of headers and neighbouring entries share requests

    def __init__(self, url, size):
        self.url, self.size, self.pos, self.cached = url, size, 0, (0, b"")

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, offset, whence=0):
        self.pos = (offset, self.pos + offset, self.size + offset)[whence]
        return self.pos

    def read(self, n=-1):
        end = self.size if n is None or n < 0 else min(self.size, self.pos + n)
        start, data = self.cached
        if not start <= self.pos <= end <= start + len(data):
            start, stop = self.pos, min(self.size, max(end, self.pos + self.BLOCK))
            with fetch(self.url, start, stop) as response:
                if response.status != 206:
                    raise OSError("The server does not send partial downloads")
                data = response.read()
            if len(data) != stop - start:
                raise OSError("A partial download came back short")
            self.cached = (start, data)
        result = data[self.pos - start:end - start]
        self.pos = end
        return result


def pick_asset(assets):
    """This platform's ZIP among a release's assets."""
    for asset in assets:
        name = asset["name"]
        if name.endswith(SUFFIX + ".zip") and (SUFFIX or not name.endswith(OTHER_PLATFORMS)):
            return asset
    raise ValueError("The release has no ZIP for this platform")


def newer(current):
    """The release (tag_name, assets) when it is newer than the installed version, else None."""
    with fetch(LATEST) as response:
        release = json.load(response)
    return release if number(release["tag_name"]) > number(current) else None


def manifest_of(assets, asset):
    """The release's <zip>.sha256.json beside asset: each entry's SHA-256, which package_test.py writes."""
    return next((other for other in assets if other["name"] == asset["name"] + ".sha256.json"), None)


def ranged_bytes(files):
    """Roughly what Ranges downloads for these entries: a read-ahead block for each one the previous block missed."""
    total = covered = 0
    for entry, _, _ in sorted(files, key=lambda file: file[0].header_offset):
        end = entry.header_offset + 30 + len(entry.filename.encode()) + len(entry.extra) + entry.compress_size
        if entry.header_offset >= covered or end > covered:
            total += max(Ranges.BLOCK, end - entry.header_offset)
            covered = max(end, entry.header_offset + Ranges.BLOCK)
    return total


def install(asset, manifest=None):
    """Fetch only changed entries when the release lists their SHA-256s under GitHub's digest; else the whole ZIP."""
    url = asset["browser_download_url"]
    files = None
    if manifest and manifest.get("digest"):
        with fetch(manifest["browser_download_url"]) as response:
            listed = response.read()
        if manifest["digest"] != "sha256:" + hashlib.sha256(listed).hexdigest():
            raise ValueError("The update's file list does not match GitHub's checksum")
        hashes = json.loads(listed)
        try:
            with zipfile.ZipFile(Ranges(url, asset["size"])) as archive:
                files = pending(archive)
                # Past half the ZIP (a new compiler, say), one whole download beats many ranges.
                # Launchers read stdout as the result, so this prints nothing.
                files = fetched(archive, files, hashes) if ranged_bytes(files) < asset["size"] // 2 else None
        except (OSError, zipfile.BadZipFile):
            files = None  # no partial downloads or a bad range: fetch the whole ZIP and check GitHub's digest
    if files is not None:
        write(files)
        return
    digest = hashlib.sha256()
    with tempfile.TemporaryFile() as download:
        with fetch(url) as response:
            while chunk := response.read(1 << 20):
                download.write(chunk)
                digest.update(chunk)
        # GitHub lists the checksum as "sha256:<hex>"; releases from before it did carry none.
        if asset.get("digest") and asset["digest"] != "sha256:" + digest.hexdigest():
            raise ValueError("The download does not match GitHub's checksum")
        with zipfile.ZipFile(download) as archive:
            unpack(archive)


def unpack(archive):
    """Writes the ZIP over the install. Its entries sit in one top folder, DOA2U-<version>/.

    Every entry is checked and read before the first write, and VERSION goes last, so an install that
    fails part way still reports the old version and can be run again."""
    write(fetched(archive, pending(archive)))


def pending(archive):
    """The (entry, target, mode) of each entry that differs from the install, judged from the ZIP's directory alone."""
    files = []
    for entry in archive.infolist():
        if entry.is_dir():
            continue
        target = (ROOT / entry.filename.partition("/")[2]).resolve()
        if ROOT not in target.parents:
            raise ValueError(f"Unsafe path in the update: {entry.filename}")
        # private/ holds the player's files; a release only adds ones they lack.
        if ROOT / "private" in target.parents and target.exists():
            continue
        mode = (entry.external_attr >> 16) & 0o777
        # Unchanged files stay untouched, including those of the bundled Python running this.
        # Windows has no mode bits beyond read-only, so only other systems compare modes.
        if (target.is_file() and target.stat().st_size == entry.file_size
                and zlib.crc32(target.read_bytes()) == entry.CRC
                and (not mode or sys.platform == "win32" or (target.stat().st_mode & 0o777) == mode)):
            continue
        files.append((entry, target, mode))
    return files


def fetched(archive, files, hashes=None):
    """(target, data, mode) for each pending entry, with VERSION last; hashes, when given, must list each entry's SHA-256."""
    result = []
    for entry, target, mode in files:
        data = archive.read(entry)
        if hashes is not None and hashlib.sha256(data).hexdigest() != hashes.get(entry.filename):
            raise ValueError(f"{entry.filename} does not match the update's file list")
        result.append((target, data, mode))
    return sorted(result, key=lambda file: file[0] == ROOT / "VERSION")


def write(files):
    for target, data, mode in files:
        target.parent.mkdir(parents=True, exist_ok=True)
        # A running exe or loaded DLL can be renamed but not overwritten; main() removes the old one next time.
        old = target.with_name(target.name + ".old")
        swap = (target.suffix in (".exe", ".dll", ".pyd") and target.exists()
                and (target.parent == ROOT or ROOT / "tools" / "python" in target.parents))
        if swap:
            target.replace(old)
        try:
            target.write_bytes(data)
        except OSError:
            if swap:
                old.replace(target)
            raise
        if mode:
            target.chmod(mode)


def main(argv):
    if len(argv) != 1 or argv[0] not in ("check", "install"):
        sys.exit("usage: update.py check|install")
    for old in [*ROOT.glob("*.exe.old"), *ROOT.glob("tools/python/**/*.old")]:
        try:
            old.unlink()
        except OSError:
            pass
    try:
        current = installed()
        if current is None:
            print("Updates are for the release ZIP; this is a source checkout")
            return 1
        release = newer(current)
        if release is not None:
            asset = pick_asset(release["assets"])
            if argv[0] == "install":
                install(asset, manifest_of(release["assets"], asset))
            print(release["tag_name"].lstrip("v"))
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print(f"Update failed: {error}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
