# SPDX-License-Identifier: GPL-3.0-or-later
"""Play the game or run a bounded probe, preserving saves and private logs."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

from build_game import ROOT, program_manifest, runner_path, selected_xbe
import dedication_card
from extract_iso import sha256

RELAUNCH = 4  # runner exit code for XLaunchNewImage of the game's own XBE
# The archives check_hdd_install (0xf6cc0) expects on Z:, copied there by hdd_install_thread.
HDD_ARCHIVES = ("voice.afs", "datahdd.afs", "datahdd2.afs")


def prepare_disc(source, disc):
    if disc.exists():
        return
    staging = disc.with_name(disc.name + ".tmp")
    if staging.exists():
        shutil.rmtree(staging)  # Left by an interrupted first launch; holds no saves.
    staging.mkdir(parents=True)
    copying = False
    try:
        for path in source.rglob("*"):
            relative = path.relative_to(source)
            if relative.parts[0] == ".recomp-storage":
                continue
            if path.is_symlink() or getattr(path, "is_junction", lambda: False)():
                raise ValueError("Disc source contains a link")
            if path.is_file():
                destination = staging / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                try:
                    os.link(path, destination)
                except OSError:
                    if not copying:
                        print("This drive has no hardlinks, so the first launch copies the disc "
                              "(about 4 GB). This can take several minutes; keep this window open.",
                              flush=True)
                        copying = True
                    shutil.copyfile(path, destination)  # FAT32 and exFAT have no hardlinks.
    except BaseException:
        shutil.rmtree(staging)  # Holds only links or copies of the import, so no saves are lost.
        raise
    staging.rename(disc)


def seed_hdd_cache(disc):
    """Copy the disc archives the game would install to Z: on first boot.

    The game's own install streams them 32 KB at a time and holds the boot for
    minutes; with matching sizes, check_hdd_install accepts the cache at once.
    Copies, not hardlinks: the installer reopens a stale file with CREATE_ALWAYS.
    """
    cache = disc / ".recomp-storage/partition5"
    for name in HDD_ARCHIVES:
        source, target = disc / "doa2" / name, cache / name
        if not source.is_file() or (target.is_file() and target.stat().st_size == source.stat().st_size):
            continue
        cache.mkdir(parents=True, exist_ok=True)
        partial = target.with_name(name + ".tmp")
        shutil.copyfile(source, partial)
        partial.replace(target)


def saved_settings():
    """The choices a launcher saved in private/launcher.json, as run() options."""
    try:
        saved = json.loads((ROOT / "private/launcher.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    options = {}
    if saved.get("height") in (480, 720, 1080, 1440, 2160):
        options["resolution"] = saved["height"]
    if saved.get("msaa") in (1, 2, 4, 8, 16, 32):
        options["msaa"] = saved["msaa"]
    for key, option in (("smaa", "smaa"), ("widescreen", "widescreen"), ("skipIntro", "skip_intro"),
                        ("surround", "surround"), ("unlockAll", "unlock_all")):
        if isinstance(saved.get(key), bool):
            options[option] = saved[key]
    return options


def open_launcher():
    """Linux builds its settings screen beside the runner (tools/launcher_sdl.cpp);
    macOS runs tools/launcher_macos.swift from launcher.sh instead."""
    launcher = runner_path(ROOT).with_name("recomp_launcher")
    if not launcher.is_file():
        raise ValueError(f"{launcher} is missing; rebuild with ./build_game.sh")
    return subprocess.run([str(launcher), str(ROOT)]).returncode == 0


def run(seconds, start_polls=(), a_polls=(), isolated=False, buttons_polls=(), analog_polls=(),
        widescreen=None, msaa=None, smaa=None, resolution=None, story_finale=False,
        skip_intro=False, unlock_all=True, surround=None):
    if seconds is not None and seconds <= 0:
        raise ValueError("Probe duration must be positive")
    if msaa is not None and msaa not in (1, 2, 4, 8, 16, 32):
        raise ValueError("MSAA must be 1, 2, 4, 8, 16 or 32")
    if resolution is not None and resolution not in (480, 720, 1080, 1440, 2160):
        raise ValueError("Resolution must be 480, 720, 1080, 1440 or 2160")
    env = os.environ.copy()
    for name, value in (("RECOMP_D3D_WIDESCREEN", widescreen),
                        ("RECOMP_D3D_MSAA", msaa), ("RECOMP_D3D_SMAA", smaa),
                        ("RECOMP_AUDIO_SURROUND", surround)):
        if value is not None:
            env[name] = str(int(value))
    if resolution is not None:
        env["RECOMP_D3D_SCALE"] = str(resolution / 480)
    # The SDL_GPU presenter keeps compiled shaders here; Direct3D 11 ignores it.
    shader_cache = ROOT / "private/shader-cache"
    shader_cache.mkdir(parents=True, exist_ok=True)
    env.setdefault("RECOMP_SHADER_CACHE", str(shader_cache))
    if len(start_polls) > 4 or any(poll <= 0 or poll > 2**64 - 1 for poll in start_polls):
        raise ValueError("Start polls must be positive uint64 values, at most four")
    if len(a_polls) > 192 or any(poll <= 0 or poll > 2**64 - 1 for poll in a_polls):
        raise ValueError("A polls must be positive uint64 values, at most 192")
    if len(buttons_polls) > 1024 or any(poll <= 0 or poll > 2**64 - 1 or mask <= 0 or mask > 255
                                       for poll, mask in buttons_polls):
        raise ValueError("Button polls require positive uint64 polls and masks 1..255, at most 1024")
    if len(analog_polls) > 1024 or any(poll <= 0 or poll > 2**64 - 1 or index < 0 or index > 7
                                      for poll, index in analog_polls):
        raise ValueError("Analog polls require positive uint64 polls and indices 0..7, at most 1024")
    receipt_path = ROOT / "private/program/receipt.json"
    if not receipt_path.is_file():
        raise ValueError('The game is not built yet. Build it from your Disc 2 ISO and wait for '
                         '"Build complete"; if it says "Build failed", report that line')
    selected = selected_xbe()
    generated = ROOT / "private/program/generated"
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    if receipt["recipe"]["xbe_sha256"] != sha256(selected) or program_manifest(generated) != (
            receipt["manifest_sha256"], receipt["ebp_count"]):
        raise ValueError("Game or generated sources differ from the build receipt")
    runner = runner_path(ROOT)
    if sha256(runner) != receipt["runner_sha256"]:
        raise ValueError("Runner differs from its successful build receipt")
    # A finale run clears Story, so it keeps its unlocks out of the player's saves.
    disc = ROOT / ("private/probe-disc" if isolated or story_finale else "private/play-disc")
    prepare_disc(selected.parent, disc)
    seed_hdd_cache(disc)
    xbe = disc / selected.name
    if sha256(xbe) != sha256(selected):
        raise ValueError("Play disc belongs to a different game import")
    card = ROOT / "private/dedication.bmp"
    photo = ROOT / "private/dedication-photo.jpg"
    if not card.is_file() and photo.is_file():
        try:
            dedication_card.build(selected.parent, photo, card)
        except (ImportError, OSError, ValueError) as error:
            print(f"Dedication card skipped: {error}", file=sys.stderr)
    if card.is_file():
        env["RECOMP_DEDICATION_CARD"] = str(card)
    logs = ROOT / ("private/play-logs" if seconds is None else "private/boot-logs")
    logs.mkdir(exist_ok=True)
    log_path = logs / (uuid.uuid4().hex[:8] + ".log")
    if seconds is None:
        print(f"Play session log={log_path}", flush=True)
    with log_path.open("x", encoding="utf-8") as log:
        log.write(f"runner_sha256={sha256(runner)}\nprogram_manifest={receipt['manifest_sha256']}\n")
        log.flush()
        try:
            command = [str(runner), "--xbe", str(xbe), "--disc-root", str(disc)]
            if isolated:
                command.append("--input-script-only")
            if story_finale:
                command.append("--story-finale")
            if skip_intro:
                command.append("--skip-intro")
            if not unlock_all:
                command.append("--no-unlock-all")
            for poll in start_polls:
                command.extend(["--input-start-pulse-at", str(poll)])
            for poll in a_polls:
                command.extend(["--input-a-pulse-at", str(poll)])
            for poll, mask in buttons_polls:
                command.extend(["--input-buttons-pulse-at", str(poll), str(mask)])
            for poll, index in analog_polls:
                command.extend(["--input-analog-pulse-at", str(poll), str(index)])
            code = RELAUNCH
            # The game's soft reset relaunches its own XBE; a fresh runner is that cold boot.
            while code == RELAUNCH:
                result = subprocess.run(command,
                                        cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=seconds,
                                        env=env)
                code = result.returncode
                if seconds is not None:
                    break
        except subprocess.TimeoutExpired:
            log.write("\nprobe timed out; child process terminated\n")
            code = 124
    print(f"exit={code} log={log_path}")
    return code


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    duration = parser.add_mutually_exclusive_group()
    duration.add_argument("--seconds", type=int, default=30)
    duration.add_argument("--play", action="store_true",
                          help="Play without a time limit; close the game window to exit")
    parser.add_argument("--widescreen", action=argparse.BooleanOptionalAction, default=None,
                        help="Use the game's native 16:9 camera and menus")
    parser.add_argument("--resolution", type=int, choices=(480, 720, 1080, 1440, 2160),
                        help="Render height: 480p, 720p, 1080p, 1440p or 4K; width follows aspect ratio")
    parser.add_argument("--msaa", type=int, choices=(1, 2, 4, 8, 16, 32),
                        help="MSAA samples (1 disables); falls back to GPU support")
    parser.add_argument("--smaa", action=argparse.BooleanOptionalAction, default=None,
                        help="Apply SMAA Ultra after MSAA resolve")
    parser.add_argument("--surround", action=argparse.BooleanOptionalAction, default=None,
                        help="Output 5.1 through the game's own surround mix")
    parser.add_argument("--input-start-pulse-at", type=int, action="append", default=[])
    parser.add_argument("--input-a-pulse-at", type=int, action="append", default=[])
    parser.add_argument("--input-buttons-pulse-at", type=int, nargs=2, action="append", default=[],
                        metavar=("POLL", "MASK"), help="Press a digital button mask (decimal)")
    parser.add_argument("--isolated", action="store_true",
                        help="Use separate probe saves and ignore physical input")
    parser.add_argument("--input-analog-pulse-at", type=int, nargs=2, action="append", default=[],
                        metavar=("POLL", "INDEX"), help="Press A/B/X/Y/Black/White/LTrig/RTrig (0..7)")
    parser.add_argument("--story-finale", action="store_true",
                        help="Debug: Story skips to its last fights with each CPU at 1 life")
    parser.add_argument("--skip-intro", action="store_true",
                        help="Skip the logo and opening movies at boot")
    parser.add_argument("--no-unlock-all", dest="unlock_all", action="store_false",
                        help="Leave character and costume unlocks to play, for acceptance runs")
    parser.add_argument("--saved", action="store_true",
                        help="Use the choices saved by the launcher in private/launcher.json")
    parser.add_argument("--launcher", action="store_true",
                        help="Open the Linux settings screen first, then play with its choices")
    args = parser.parse_args()
    try:
        if args.launcher and not open_launcher():
            raise SystemExit(0)
        saved = saved_settings() if args.saved or args.launcher else {}
        def pick(name, value):
            return value if value is not None else saved.get(name)
        raise SystemExit(run(None if args.play else args.seconds, args.input_start_pulse_at,
                             args.input_a_pulse_at, args.isolated, args.input_buttons_pulse_at,
                             args.input_analog_pulse_at, pick("widescreen", args.widescreen),
                             pick("msaa", args.msaa), pick("smaa", args.smaa),
                             pick("resolution", args.resolution), args.story_finale,
                             args.skip_intro or saved.get("skip_intro", False),
                             args.unlock_all and saved.get("unlock_all", True),
                             pick("surround", args.surround)))
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f"Cannot run game: {error}", file=sys.stderr)
        raise SystemExit(1)
