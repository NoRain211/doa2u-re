# SPDX-License-Identifier: GPL-3.0-or-later
"""Find the pinned llvm-mingw compiler, ffmpeg on Windows and, on x86-64 Linux, extract-xiso,
downloading each when missing."""
# Adapted from committed DOAXBV tools/build_prerequisites.py at 5761e89.
import os
from pathlib import Path
import platform
import shutil
import ssl
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile

from extract_iso import sha256

LLVM_MINGW_URL = ("https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/"
                  "llvm-mingw-20261006-ucrt-x86_64.zip")
LLVM_MINGW_SHA256 = "317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a"
LLVM_MINGW = Path("tools/llvm-mingw")
# Upstream's Linux binary is x86-64 and needs glibc 2.34 or newer.
# shortcut: macOS is not fetched because upstream's binary is arm64 only; add it if one covers Intel Macs.
EXTRACT_XISO_URL = ("https://github.com/XboxDev/extract-xiso/releases/download/build-202609111233/"
                    "extract-xiso_Linux.zip")
EXTRACT_XISO_SHA256 = "c4b9fbf86b57c74d07fb643522740ae82034d8280ccde24528752cd1cb0b798d"


# ffmpeg decodes DOA4's XMA audio for tools/import_doa4_voices.py and tools/import_soundtrack.py.
# Gyan Doshi's essentials build (GPL, as this project; static, with the xma1 decoder), mirrored on GitHub.
FFMPEG_URL = ("https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/"
              "ffmpeg-9.0.2-essentials_build.zip")
FFMPEG_SHA256 = "60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba"
FFMPEG = Path("tools/ffmpeg")


def runs_upstream_linux_binary():
    """True on x86-64 Linux with glibc 2.34 or newer, which upstream's Linux binary needs."""
    if not sys.platform.startswith("linux") or platform.machine() not in ("x86_64", "AMD64"):
        return False
    libc, version = platform.libc_ver()
    try:
        return libc == "glibc" and tuple(map(int, version.split(".")[:2])) >= (2, 34)
    except ValueError:
        return False


def fetch_verified(url, expected_sha256, archive, name, manual=""):
    try:
        import truststore  # the Windows release bundles it; elsewhere Python's own CA store is used
        truststore.inject_into_ssl()
    except ImportError:
        pass
    try:
        with urllib.request.urlopen(url, timeout=60) as response, archive.open("xb") as file:
            shutil.copyfileobj(response, file)
    except (OSError, urllib.error.URLError) as error:
        message = f"Could not download {name}: {error}. Retry when online."
        if isinstance(getattr(error, "reason", error), ssl.SSLCertVerificationError):
            # Antivirus HTTPS scanning, a VPN or a proxy usually re-signs the connection with a root Python lacks.
            message += (f" If antivirus HTTPS scanning, a VPN or a proxy is on, pause it and rerun, or download "
                        f"{url} in your browser. {manual}").rstrip()
        raise ValueError(message) from error
    if sha256(archive) != expected_sha256:
        raise ValueError(f"The {name} download failed its SHA-256 check. Rerun the build.")


def install_llvm_mingw(root):
    target = root / LLVM_MINGW
    if target.exists():
        raise ValueError(f"{target} exists but has no bin/clang.exe. Delete it and rerun the build.")
    private = root / "private"
    private.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="llvm-mingw-", dir=private) as folder:
        archive = Path(folder) / "llvm-mingw.zip"
        print("Downloading the llvm-mingw compiler (190 MB)...", flush=True)
        fetch_verified(LLVM_MINGW_URL, LLVM_MINGW_SHA256, archive, "llvm-mingw",
                       f"Extract the folder inside it into {target} so {target / 'bin/clang.exe'} exists.")
        print("Extracting llvm-mingw...", flush=True)
        staging = Path(folder) / "extract"
        with zipfile.ZipFile(archive) as contents:
            contents.extractall(staging)
        (top,) = staging.iterdir()  # the release ZIP holds one versioned folder
        target.parent.mkdir(parents=True, exist_ok=True)
        # Antivirus scans of the fresh executables can briefly lock the folder against renaming.
        for attempt in range(30):
            try:
                top.replace(target)
                break
            except PermissionError:
                if attempt == 29:
                    raise
                time.sleep(1)


def llvm_mingw(root):
    """Return clang.exe (pinned copy, else a llvm-mingw on PATH) and put its folder, which also holds mingw32-make, first on PATH."""
    clang = root / LLVM_MINGW / "bin/clang.exe"
    if not clang.is_file():
        # shortcut: a PATH toolchain skips the pin and SHA-256 check; it must ship mingw32-make like llvm-mingw does.
        found = shutil.which("clang.exe") if sys.platform == "win32" else None
        if found and (Path(found).parent / "mingw32-make.exe").is_file():
            clang = Path(found)
    if not clang.is_file():
        install_llvm_mingw(root)
        if not clang.is_file():
            raise ValueError(f"llvm-mingw was extracted, but {clang} is missing; extract {LLVM_MINGW_URL} there.")
    path = os.environ.get("PATH", "").split(os.pathsep)
    os.environ["PATH"] = os.pathsep.join([str(clang.parent)] + [p for p in path if p != str(clang.parent)])
    return clang


def extract_xiso(root):
    """Return the extract-xiso to run: the one on PATH, else on x86-64 glibc Linux the pinned upstream build."""
    if shutil.which("extract-xiso") or not runs_upstream_linux_binary():
        return "extract-xiso"
    # Keyed by the pin, so a new pin downloads a new binary.
    tool = root / f"private/extract-xiso-{EXTRACT_XISO_SHA256[:12]}"
    if not tool.is_file():
        (root / "private").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="extract-xiso-", dir=root / "private") as folder:
            archive, staged = Path(folder) / "extract-xiso.zip", Path(folder) / "extract-xiso"
            print("Downloading extract-xiso...", flush=True)
            fetch_verified(EXTRACT_XISO_URL, EXTRACT_XISO_SHA256, archive, "extract-xiso")
            with zipfile.ZipFile(archive) as contents:
                staged.write_bytes(contents.read("extract-xiso"))
            staged.chmod(0o755)
            staged.replace(tool)
    return tool


def find_ffmpeg(root):
    """The ffmpeg the build fetched (tools/ffmpeg), else the one on PATH."""
    bundled = Path(root) / FFMPEG / "ffmpeg.exe"
    return str(bundled) if bundled.is_file() else (shutil.which("ffmpeg") or "ffmpeg")


def ffmpeg(root):
    """On Windows, fetch the pinned ffmpeg into tools/ffmpeg once (ffmpeg.exe and its licence); return find_ffmpeg."""
    target = Path(root) / FFMPEG
    if sys.platform == "win32" and not (target / "ffmpeg.exe").is_file():
        if target.exists():
            raise ValueError(f"{target} exists but has no ffmpeg.exe. Delete it and rerun.")
        (Path(root) / "private").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="ffmpeg-", dir=Path(root) / "private") as folder:
            archive = Path(folder) / "ffmpeg.zip"
            print("Downloading ffmpeg for DOA4's audio (115 MB)...", flush=True)
            fetch_verified(FFMPEG_URL, FFMPEG_SHA256, archive, "ffmpeg")
            staging = Path(folder) / "ffmpeg"
            staging.mkdir()
            with zipfile.ZipFile(archive) as contents:
                for name in contents.namelist():
                    if name.endswith(("/bin/ffmpeg.exe", "/LICENSE", "/README.txt")):
                        (staging / name.rsplit("/", 1)[1]).write_bytes(contents.read(name))
            if not (staging / "ffmpeg.exe").is_file():
                raise ValueError(f"{FFMPEG_URL} holds no bin/ffmpeg.exe")
            target.parent.mkdir(parents=True, exist_ok=True)
            for attempt in range(30):
                try:
                    staging.replace(target)
                    break
                except PermissionError:
                    if attempt == 29:
                        raise
                    time.sleep(1)
    return find_ffmpeg(root)
