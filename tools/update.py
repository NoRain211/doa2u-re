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


def fetch(url):
    return urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "DOA2U-launcher"}), timeout=30)


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


def install(asset):
    digest = hashlib.sha256()
    with tempfile.TemporaryFile() as download:
        with fetch(asset["browser_download_url"]) as response:
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
        files.append((target, archive.read(entry), (entry.external_attr >> 16) & 0o777))
    files.sort(key=lambda file: file[0] == ROOT / "VERSION")
    for target, data, mode in files:
        target.parent.mkdir(parents=True, exist_ok=True)
        # A running exe can be renamed but not overwritten; main() removes the old one next time.
        old = target.with_name(target.name + ".old")
        swap = target.suffix == ".exe" and target.parent == ROOT and target.exists()
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
    for old in ROOT.glob("*.exe.old"):
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
                install(asset)
            print(release["tag_name"].lstrip("v"))
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print(f"Update failed: {error}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
