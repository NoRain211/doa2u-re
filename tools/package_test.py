# SPDX-License-Identifier: GPL-3.0-or-later
"""Package a release ZIP (e.g. 0.1-Alpha): source, the pinned lifter and setup tools, no game data."""
# Adapted from committed DOA3 tools/package_test.py at 5747204.
import argparse
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SUBMODULE = "tools/xboxrecomp"
# Agent notes and dev-only submodule metadata stay out of the tester package.
EXCLUDE = {"AGENTS.md", ".gitmodules", SUBMODULE}
# extract-xiso.exe and its LICENSE.TXT, copied from the DOA3 test package, and
# dedication-photo.jpg, which run_game.py turns into the staff-roll card.
ARTIFACTS = ROOT / "private" / "release-assets"
# ZIP name suffix and README for each platform; macOS and Linux build extract-xiso themselves.
PLATFORMS = {"windows": ("", "docs/test-release.md"),
             "macos": ("-macOS", "docs/test-release-macos.md"),
             "linux": ("-Linux", "docs/test-release-linux.md")}


# The C# compiler every Windows ships builds both launchers from tools/launcher.cs.
CSC = Path(os.environ.get("WINDIR", r"C:\Windows")) / "Microsoft.NET" / "Framework64" / "v4.0.30319" / "csc.exe"
LAUNCHERS = {"Launcher.exe": [], "DOA2U.exe": ["/define:PLAY_ONLY"]}
# Optional private icon (fan art from issue #39 holds Tecmo's character and logo art, so it stays untracked).
ICON = ROOT / "private" / "launcher-icon.ico"


def build_launchers(out_dir):
    """Launcher.exe (build, settings, play) and DOA2U.exe (play) into out_dir."""
    if not CSC.is_file():
        raise SystemExit(f"The launchers need Windows: {CSC} is missing")
    icon = [f"/win32icon:{ICON}"] if ICON.is_file() else []
    for exe, flags in LAUNCHERS.items():
        subprocess.run([str(CSC), "/nologo", "/warnaserror", "/target:winexe", "/r:System.Web.Extensions.dll",
                        f"/resource:{ROOT / 'tools' / 'launcher-background.jpg'},launcher-background.jpg",
                        *icon, *flags, f"/out:{out_dir / exe}", str(ROOT / "tools" / "launcher.cs")], check=True)


def git(*args, cwd=ROOT):
    return subprocess.check_output(["git", *args], cwd=cwd)


def archive(cwd, prefix):
    with tarfile.open(fileobj=io.BytesIO(git("archive", "--format=tar", "HEAD", cwd=cwd))) as tar:
        for member in tar.getmembers():
            if member.isfile() and member.name not in EXCLUDE:
                # Keep Git's file mode so the shell scripts stay executable after unzipping.
                info = zipfile.ZipInfo(prefix + member.name, time.localtime()[:6])
                info.compress_type = zipfile.ZIP_DEFLATED
                info.create_system = 3  # Unix, or unzip ignores the mode below
                info.external_attr = (0o100000 | member.mode) << 16
                yield info, tar.extractfile(member).read()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", nargs="?")
    parser.add_argument("--platform", choices=PLATFORMS, default="windows")
    parser.add_argument("--launchers", type=Path, metavar="DIR",
                        help="only build Launcher.exe and DOA2U.exe into DIR (the repository root for a checkout)")
    args = parser.parse_args()
    if args.launchers:
        build_launchers(args.launchers)
        return
    if not args.version:
        parser.error("give a version, or --launchers DIR")
    suffix, readme = PLATFORMS[args.platform]
    if git("status", "--porcelain", "--untracked-files=no").strip():
        raise SystemExit("Commit first: the package is built from HEAD")
    pinned = git("ls-tree", "HEAD", SUBMODULE).split()[2].decode()
    if git("rev-parse", "HEAD", cwd=ROOT / SUBMODULE).decode().strip() != pinned or \
            git("status", "--porcelain", cwd=ROOT / SUBMODULE).strip():
        raise SystemExit(f"{SUBMODULE} must be a clean checkout of {pinned}")

    name = f"DOA2U-{args.version}"
    out = ROOT / "private" / "release" / f"{name}{suffix}.zip"
    if out.exists():
        raise SystemExit(f"{out} already exists")
    windows = args.platform == "windows"
    artifacts = ("extract-xiso.exe", "LICENSE.TXT", "dedication-photo.jpg")
    missing = [a for a in artifacts if not (ARTIFACTS / a).is_file()]
    if missing:
        raise SystemExit(f"Missing from {ARTIFACTS}: {', '.join(missing)}")
    out.parent.mkdir(parents=True, exist_ok=True)
    partial = out.with_suffix(".zip.tmp")
    with zipfile.ZipFile(partial, "w", zipfile.ZIP_DEFLATED) as package:
        for path, data in archive(ROOT, f"{name}/"):
            package.writestr(path, data)
        for path, data in archive(ROOT / SUBMODULE, f"{name}/{SUBMODULE}/"):
            package.writestr(path, data)
        if windows:
            for artifact in artifacts[:2]:
                package.write(ARTIFACTS / artifact, f"{name}/tools/artifacts/{artifact}")
            with tempfile.TemporaryDirectory() as launchers:
                build_launchers(Path(launchers))
                for exe in LAUNCHERS:
                    package.write(Path(launchers) / exe, f"{name}/{exe}")
        package.write(ARTIFACTS / artifacts[2], f"{name}/private/{artifacts[2]}")
        package.write(ROOT / readme, f"{name}/README.txt")
    partial.rename(out)
    print(out)


if __name__ == "__main__":
    main()
