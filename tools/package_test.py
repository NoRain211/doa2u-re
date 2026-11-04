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
# A release holds what building and playing need; developer and agent material stays in the repository.
DEV_FILES = {"AGENTS.md", "CONTEXT.md", "README.md", ".gitattributes", ".gitignore", ".gitmodules",
             "private/README.md", "tools/package_test.py", "tools/public_export.py", "tools/xbe_info.py",
             "tools/doa2u/function_status.json", SUBMODULE}
DEV_PREFIXES = (".github/", "docs/", "tools/test_", "tools/doa2u/ghidra/")
# Files one platform family alone builds or runs; recomp-runtime/CMakeLists.txt picks backends by WIN32.
WINDOWS_ONLY = {"tools/launcher.cs", *(f"recomp-runtime/{name}" for name in (
    "audio_output_xaudio2.cpp", "audio_output_xaudio2_test.cpp", "audio_output_xaudio2_host_test.cpp",
    "audio_xtalk_xapo.cpp", "audio_xtalk_xapo.h", "audio_capture_xapo.cpp", "audio_capture_xapo.h",
    "audio_capture_xapo_test.cpp", "xaudio2_reverb.h", "xaudio2_reverb_test.cpp",
    "d3d_presenter_d3d11.cpp", "d3d_presenter_thread.cpp"))}
POSIX_ONLY = {"build_game.sh", "launcher.sh", "run_game.sh", "docs/building.md", *(f"recomp-runtime/{name}" for name in (
    "audio_output_sdl.cpp", "audio_output_sdl_test.cpp", "audio_reverb.cpp", "audio_reverb.h",
    "d3d_presenter_sdl_gpu.cpp", "d3d_presenter_inline.cpp", "d3d_shader_compiler.cpp", "d3d_shader_compiler.h",
    "win32_compat.c", "win32_compat_test.c", "fiber_switch_arm64.S", "fiber_switch_x86_64.S"))}
PLATFORM_FILES = {"windows": WINDOWS_ONLY,
                  "macos": POSIX_ONLY | {"tools/launcher_macos.swift", "tools/launcher-background.jpg"},
                  "linux": POSIX_ONLY | {"tools/launcher_sdl.cpp", "tools/launcher-background.png"}}
# The build runs only the lifter's Python lift (tools.recomp, which loads tools.disasm and tools.xbe_parser
# and copies its register-model header beside the program).
LIFTER_DIRS = ("tools/recomp/", "tools/disasm/", "tools/xbe_parser/")
LIFTER_FILES = {"LICENSE", "NOTICE", "tools/__init__.py", "templates/runtime/recomp_types.h"}
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


def shipped(path, platform):
    """Whether a repository file belongs in the platform's release."""
    if path in PLATFORM_FILES[platform]:
        return True
    if any(path in files for files in PLATFORM_FILES.values()):
        return False
    return path not in DEV_FILES and not path.startswith(DEV_PREFIXES)


def lifter_shipped(path):
    """Whether a lifter file belongs in a release: its licenses and the lift's sources, without tests or notes."""
    if path in LIFTER_FILES or path.startswith("LICENSES/"):
        return True
    name = path.rsplit("/", 1)[-1]
    return path.startswith(LIFTER_DIRS) and not name.startswith("test_") and not name.endswith(".md")


def archive(cwd, prefix, keep):
    with tarfile.open(fileobj=io.BytesIO(git("archive", "--format=tar", "HEAD", cwd=cwd))) as tar:
        for member in tar.getmembers():
            if member.isfile() and keep(member.name):
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
        for path, data in archive(ROOT, f"{name}/", lambda path: shipped(path, args.platform)):
            package.writestr(path, data)
        lifter_prefix = f"{name}/{SUBMODULE}/"
        lifter = set()
        for path, data in archive(ROOT / SUBMODULE, lifter_prefix, lifter_shipped):
            package.writestr(path, data)
            lifter.add(path.filename[len(lifter_prefix):])
        # A pin that moves part of the lift must not ship a ZIP that cannot build.
        missing = sorted(LIFTER_FILES - lifter) + [d for d in LIFTER_DIRS if not any(p.startswith(d) for p in lifter)]
        if missing:
            raise SystemExit(f"{SUBMODULE} at its pin lacks {', '.join(missing)}; update the lifter lists")
        if windows:
            for artifact in artifacts[:2]:
                package.write(ARTIFACTS / artifact, f"{name}/tools/artifacts/{artifact}")
            with tempfile.TemporaryDirectory() as launchers:
                build_launchers(Path(launchers))
                for exe in LAUNCHERS:
                    package.write(Path(launchers) / exe, f"{name}/{exe}")
        package.write(ARTIFACTS / artifacts[2], f"{name}/private/{artifacts[2]}")
        package.write(ROOT / readme, f"{name}/README.txt")
        package.writestr(f"{name}/VERSION", args.version)  # tools/update.py compares it with the newest release
    partial.rename(out)
    print(out)


if __name__ == "__main__":
    main()
