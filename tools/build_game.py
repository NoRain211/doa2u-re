# SPDX-License-Identifier: GPL-3.0-or-later
"""Lift the privately selected game executable and build its native runner."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time
import uuid

from extract_iso import extract, run_logged, sha256
from build_prerequisites import extract_xiso, llvm_mingw

ROOT = Path(__file__).resolve().parents[1]
LIFTER = ROOT / "tools/xboxrecomp"
XBE_SHA256 = "30d5bd3ff363afb72ea4d5058537013e284df0930f1921d345aeaf44af288a0c"
CAPSTONE = "5.0.7"
PILLOW = "12.2.0"  # minimum; run_game.py builds the optional dedication card with it
# The generated program the runtime was validated against. Changing the lifter
# pin or the function list in tools/doa2u/functions changes it; regenerate and update both.
PROGRAM_SHA256 = "5f43bf496a7b2e22c66598ed75eda161220eb420b55e833e5f3e70ce84ad1760"
PROGRAM_EBP = 0
# The disassembler's function list, minus call edges the lift does not need, so players skip disassembly.
FUNCTIONS = ROOT / "tools/doa2u/functions"
# Seconds and output lines each step took in the validated build; they pace the progress bar.
# The build starts every generated file at once, so the compile advances by time alone.
STEPS = {"xbe-parser": (5, 253), "disasm": (1440, 33), "lift": (315, 55),
         "configure": (30, 20), "build": (245, 0)}
LABELS = {"xbe-parser": "Reading the executable", "disasm": "Disassembling",
          "lift": "Translating to C", "configure": "Configuring", "build": "Compiling"}


def build_fraction(plan, finished, lines, elapsed):
    """Share of the planned steps done: finished ones plus the current one's output or time."""
    total = sum(STEPS[step][0] for step in plan)
    done = sum(STEPS[step][0] for step in plan[:finished])
    if finished < len(plan):
        seconds, expected = STEPS[plan[finished]]
        # Time alone stops short of the step's end, so a slower PC waits on real output.
        share = min(elapsed / seconds, 0.95)
        done += seconds * min(max(lines / expected, share) if expected else share, 0.99)
    return done / total


def failure_excerpt(lines, size=20):
    """The log's end, led by the first error when it lies earlier: CMake prints its cause before a long help."""
    end = max(len(lines) - size, 0)
    first = next((i for i, line in enumerate(lines[:end]) if "error" in line.lower()), None)
    if first is None:
        return lines[end:]
    if first + size >= end:
        return lines[first:]
    return [*lines[first:first + size], "...", *lines[end:]]


class Progress:
    """One console bar across the build steps; each step's output still goes to its log."""

    def __init__(self, plan):
        self.plan, self.finished, self.lines = plan, 0, 0
        self.start = self.step_start = time.monotonic()

    def draw(self):
        now = time.monotonic()
        share = build_fraction(self.plan, self.finished, self.lines, now - self.step_start)
        label = LABELS[self.plan[min(self.finished, len(self.plan) - 1)]]
        minutes, seconds = divmod(int(now - self.start), 60)
        print(f"\r[{'#' * int(share * 30):<30}] {share:4.0%}  {label:<22} {minutes:02d}:{seconds:02d} elapsed",
              end="", flush=True)

    def count_line(self):
        self.lines += 1

    def tick(self, stop):
        while not stop.wait(1):
            self.draw()

    def run(self, args, log_path, cwd=None):
        if sys.stdout.isatty():
            self.run_with_bar(args, log_path, cwd)
        else:
            run_logged(args, log_path, cwd)
            self.finished += 1

    def run_with_bar(self, args, log_path, cwd):
        stop = threading.Event()
        ticker = threading.Thread(target=self.tick, args=(stop,), daemon=True)
        self.draw()
        ticker.start()
        try:
            run_logged(args, log_path, cwd, on_line=self.count_line)
        except subprocess.CalledProcessError:
            stop.set()
            ticker.join()
            excerpt = failure_excerpt(log_path.read_text(encoding="utf-8", errors="replace").splitlines())
            print(f"\n{LABELS[self.plan[self.finished]]} failed. Excerpt of {log_path}:", *excerpt, sep="\n")
            raise
        finally:
            stop.set()
            ticker.join()
        self.finished, self.lines, self.step_start = self.finished + 1, 0, time.monotonic()
        self.draw()
        if self.finished == len(self.plan):
            print()


def selected_xbe():
    target = json.loads((ROOT / "private/target.json").read_text(encoding="utf-8"))
    xbe = (ROOT / target["xbe_path"]).resolve(strict=True)
    if not xbe.is_relative_to((ROOT / "private").resolve()) or not xbe.is_file():
        raise ValueError("Selected executable must be a file under private/")
    if sha256(xbe) != target["xbe_sha256"]:
        raise ValueError("Selected executable differs from its private identity receipt")
    return xbe


def use_windows_newlines(generated):
    """PROGRAM_SHA256 authenticates the lifter's Windows output, which Python
    writes with CRLF line endings; other hosts write LF, so match those bytes."""
    if sys.platform == "win32":
        return
    for path in generated.iterdir():
        if path.is_file():
            data = path.read_bytes()
            path.write_bytes(data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))


def runner_path(root=ROOT):
    """The Windows runner builds with llvm-mingw in its own folder, so an old MSVC cache cannot clash."""
    if sys.platform == "win32":
        return root / "build/recomp-program-mingw/recomp_program_runner.exe"
    return root / "build/recomp-program/recomp_program_runner"


def program_manifest(generated):
    chunks = sorted(generated.glob("recomp_[0-9][0-9][0-9][0-9].c"))
    if not chunks or [p.name for p in chunks] != [f"recomp_{i:04d}.c" for i in range(len(chunks))]:
        raise ValueError("Generated program has no chunks or a gap in its chunk sequence")
    members = chunks + [generated / "recomp_dispatch.c", generated / "recomp_funcs.h"]
    manifest = "".join(f"{p.name}\t{p.stat().st_size}\t{sha256(p)}\n" for p in members)
    ebp = sum(p.read_text(encoding="utf-8").count("\n    uint32_t ebp;\n")
              for p in members if p.suffix == ".c")
    return hashlib.sha256(manifest.encode("utf-8")).hexdigest(), ebp


def lifter_revision():
    if not (LIFTER / ".git").exists():
        # A release ZIP ships the pinned lifter as plain files, without Git.
        manifest = json.loads((ROOT / "public-export.json").read_text(encoding="utf-8"))
        return next(link["commit"] for link in manifest["gitlinks"] if link["path"] == "tools/xboxrecomp")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=LIFTER, text=True).strip()
    if subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"], cwd=LIFTER):
        raise ValueError("Commit lifter changes and update its pin before generating")
    return revision


def merge_functions(path):
    """Join the committed function shards into the lifter's functions.json."""
    functions = [entry for shard in sorted(FUNCTIONS.glob("*.json"))
                 for entry in json.loads(shard.read_text(encoding="utf-8"))]
    path.write_text(json.dumps(functions), encoding="utf-8")


def write_functions(disasm):
    """Replace the committed shards with a fresh disassembly, under the public file-size limit."""
    functions = json.loads((disasm / "functions.json").read_text(encoding="utf-8"))
    for old in FUNCTIONS.glob("*.json"):
        old.unlink()
    for index in range(0, len(functions), 1000):
        rows = [json.dumps({k: v for k, v in entry.items() if k not in ("calls_to", "called_by")})
                for entry in functions[index:index + 1000]]
        (FUNCTIONS / f"{index // 1000:02d}.json").write_text(
            "[\n" + ",\n".join(rows) + "\n]\n", encoding="utf-8", newline="\n")


def import_disc(iso, extractor=None):
    """Extract a user's ISO once into private/imported-disc and select its game executable."""
    if not iso.is_file():
        raise ValueError("Choose your Dead or Alive Ultimate Disc 2 ISO and build again")
    imported = ROOT / "private/imported-disc"
    receipt = imported / "receipt.json"
    if receipt.is_file():
        print("Checking the ISO against the existing extraction...", flush=True)
        if json.loads(receipt.read_text(encoding="utf-8"))["iso_sha256"] != sha256(iso):
            raise ValueError(f"{imported} holds a different disc; move it away to use this ISO")
        print(f"Using the disc already extracted to {imported / 'disc'}", flush=True)
    else:
        if imported.exists():
            print(f"Removing the unfinished extraction in {imported}", flush=True)
            shutil.rmtree(imported)
        bundled = ROOT / "tools/artifacts/extract-xiso.exe"
        extract(iso, imported, extractor or (bundled if bundled.is_file() else extract_xiso(ROOT)))
    for xbe in sorted((imported / "disc").glob("*.xbe")):
        if sha256(xbe) == XBE_SHA256:
            path = ROOT / "private/target.json"
            target = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}
            target.update(xbe_path=xbe.relative_to(ROOT).as_posix(), xbe_sha256=XBE_SHA256)
            path.write_text(json.dumps(target, indent=2) + "\n", encoding="utf-8")
            return
    raise ValueError("This build supports only Dead or Alive Ultimate (Xbox, USA) Disc 2")


def build(build_only=False, lift_only=False, iso=None, disassemble=False, extractor=None):
    windows = sys.platform == "win32"
    if iso is not None:
        import_disc(iso, extractor)
        if shutil.which("cmake") is None:
            raise ValueError("Install CMake 3.20 or newer and add it to PATH")
        if not windows and shutil.which("ninja") is None:
            raise ValueError("Install Ninja and add it to PATH")
        missing = []
        try:
            import capstone
            if capstone.__version__ != CAPSTONE:
                missing.append(f"capstone=={CAPSTONE}")
        except ImportError:
            missing.append(f"capstone=={CAPSTONE}")
        if missing:
            subprocess.check_call([sys.executable, "-m", "pip", "install", *missing])
            if "capstone" in sys.modules:
                raise ValueError(f"Updated Capstone to {CAPSTONE}; run the build again")
        try:
            import PIL  # noqa: F401
        except ImportError:
            # Pillow only draws the dedication card, which run_game.py skips without it, so a
            # Python with no prebuilt Pillow (pip would otherwise try a source build) must not stop the build.
            if subprocess.call([sys.executable, "-m", "pip", "install", "--only-binary", "pillow",
                                f"pillow>={PILLOW}"]):
                print("Could not install Pillow, so the dedication card will be skipped. Check the pip error above;"
                      " a new Python version may have no prebuilt Pillow yet.")
    # Fetch the compiler before the long lift, so a failed download stops early.
    compiler = llvm_mingw(ROOT) if windows and not lift_only else None
    import capstone
    xbe = selected_xbe()
    revision = lifter_revision()
    work = ROOT / "private/program"
    work.mkdir(parents=True, exist_ok=True)
    logs = ROOT / "private/build-logs" / uuid.uuid4().hex[:8]
    logs.mkdir(parents=True)
    generated, disasm, functions = work / "generated", work / "disasm", work / "functions.json"
    receipt_path = work / "receipt.json"
    lift = [] if build_only else ["xbe-parser", "disasm", "lift"] if disassemble else ["lift"]
    progress = Progress(lift + ([] if lift_only else ["configure", "build"]))
    if disassemble:
        analysis = work / "analysis.json"
        progress.run([sys.executable, "-u", "-m", "tools.xbe_parser", xbe, "--json", analysis],
                     logs / "xbe-parser.log", LIFTER)
        progress.run([sys.executable, "-u", "-m", "tools.disasm", xbe, "-o", disasm,
                      "--analysis-json", analysis,
                      "--seed-functions", ROOT / "tools/doa2u/seed_functions.json"],
                     logs / "disasm.log", LIFTER)
        write_functions(disasm)
    merge_functions(functions)
    recipe = dict(xbe_sha256=sha256(xbe), lifter_commit=revision, capstone=capstone.__version__,
                  functions_sha256=sha256(functions))
    if build_only:
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        if receipt["recipe"] != recipe:
            raise ValueError("Generated receipt belongs to a different executable or lifter recipe")
        if program_manifest(generated) != (receipt["manifest_sha256"], receipt["ebp_count"]):
            raise ValueError("Generated program differs from its authenticated receipt")
    else:
        progress.run([sys.executable, "-u", "-m", "tools.recomp", xbe, "--all", "--split", "1000",
                    "--game-name", "Dead or Alive 2 Ultimate", "--disasm-dir", work / "none",
                    "--functions", functions,
                    "--func-id-dir", work / "none", "--abi-dir", work / "none",
                    "--icall-sites", work / "none.json", "--gen-dir", generated,
                    "--output-dir", work / "metadata"], logs / "lift.log", LIFTER)
        use_windows_newlines(generated)
        manifest, ebp = program_manifest(generated)
        if iso is not None and (manifest, ebp) != (PROGRAM_SHA256, PROGRAM_EBP):
            raise ValueError(f"Generated program {manifest} differs from the validated {PROGRAM_SHA256}")
        receipt = dict(recipe=recipe, manifest_sha256=manifest, ebp_count=ebp)
        receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    if lift_only:
        return
    build_dir = runner_path().parent
    if windows:
        generator = ["-G", "MinGW Makefiles", "-DCMAKE_BUILD_TYPE=Release",
                     f"-DCMAKE_C_COMPILER={compiler.as_posix()}",
                     f"-DCMAKE_CXX_COMPILER={compiler.with_name('clang++.exe').as_posix()}"]
    else:
        generator = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"]
    if sys.platform.startswith("linux"):
        generator.append("-DRECOMP_FETCH_SDL3=ON")  # used only when the system has no SDL3
    progress.run(["cmake", "-S", ROOT / "recomp-runtime", "-B", build_dir, *generator,
                f"-DRECOMP_PROGRAM_DIR={generated.as_posix()}",
                f"-DRECOMP_XBE_SHA256={recipe['xbe_sha256']}",
                f"-DRECOMP_PROGRAM_MANIFEST_SHA256={receipt['manifest_sha256']}",
                f"-DRECOMP_PROGRAM_EBP_EXPECTED={receipt['ebp_count']}"], logs / "configure.log")
    # Linux builds the settings launcher beside the runner; SteamOS has no compiler.
    targets = ["recomp_program_runner"] + (["recomp_launcher"] if sys.platform.startswith("linux") else [])
    # Generated chunks are large; outside Windows more jobs than this can exhaust memory.
    jobs = str(os.cpu_count() or 4) if windows else str(min(4, os.cpu_count() or 1))
    progress.run(["cmake", "--build", build_dir, "--parallel", jobs,
                "--target", *targets], logs / "build.log")
    receipt["runner_sha256"] = sha256(runner_path())
    receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path, nargs="?",
                        help="Import this Disc 2 ISO and build the validated program")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--build-only", action="store_true", help="Rebuild authenticated generated sources")
    modes.add_argument("--lift-only", action="store_true", help="Generate without building the runner")
    modes.add_argument("--disassemble", action="store_true",
                       help="Rewrite tools/doa2u/functions from a fresh disassembly, then build")
    parser.add_argument("--extractor", help="extract-xiso to use instead of the one on PATH")
    args = parser.parse_args()
    try:
        build(args.build_only, args.lift_only, args.iso, args.disassemble, args.extractor)
    except (OSError, ValueError, KeyError, ImportError, subprocess.CalledProcessError) as error:
        print(f"Build failed: {error}", file=sys.stderr)
        return 1
    if args.iso is not None:
        print("Build complete. " + ("Run DOA2U.exe to play." if sys.platform == "win32"
                                    else "Run ./launcher.sh to play."))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
