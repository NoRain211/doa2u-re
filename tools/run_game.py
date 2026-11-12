# SPDX-License-Identifier: GPL-3.0-or-later
"""Play the game or run a bounded probe, preserving saves and private logs."""
import argparse
import json
import os
from pathlib import Path
import shutil
import string
import subprocess
import sys
import threading
import uuid

from build_game import ROOT, program_manifest, runner_path, selected_xbe
import dedication_card
from extract_iso import redact_private, sha256

RELAUNCH = 4  # runner exit code for XLaunchNewImage of the game's own XBE
# The archives check_hdd_install (0xf6cc0) expects on Z:, copied there by hdd_install_thread.
HDD_ARCHIVES = ("voice.afs", "datahdd.afs", "datahdd2.afs")
# Pad buttons and keys that private/keyboard.json may bind; the codes are Windows virtual keys.
# F10 is left out because Windows treats it as a system key, and F11 because the presenter
# keeps it, with Alt+Enter, for fullscreen.
KEY_ACTIONS = ("up", "down", "left", "right", "start", "back", "a", "b", "x", "y", "white", "black", "ltrig", "rtrig")
KEY_CODES = {**{c: ord(c) for c in string.ascii_uppercase + string.digits},
             **{f"F{n}": 0x6f + n for n in range(1, 13) if n not in (10, 11)},
             "BACKSPACE": 0x08, "TAB": 0x09, "ENTER": 0x0d, "SHIFT": 0x10, "CTRL": 0x11, "ESC": 0x1b,
             "SPACE": 0x20, "LEFT": 0x25, "UP": 0x26, "RIGHT": 0x27, "DOWN": 0x28}
# Host pad buttons that private/controller.json may bind, as input_host_win32.c names them;
# A, B, X and Y are by position, so Cross is A on a PlayStation pad.
PAD_NAMES = ("A", "B", "X", "Y", "Back", "Start", "LB", "RB", "LT", "RT", "LS", "RS",
             "Up", "Down", "Left", "Right", "Misc", "P1", "P2", "P3", "P4", "Touchpad")


def bindings_file(name, example):
    """The object in private/<name>, which the game's Rebind screen also writes; {} if absent."""
    try:
        bindings = json.loads((ROOT / "private" / name).read_text(encoding="utf-8"))
    except FileNotFoundError:
        return {}
    except (OSError, ValueError) as error:
        raise ValueError(f"private/{name} is not valid JSON: {error}")
    if not isinstance(bindings, dict):
        raise ValueError(f"private/{name} must be an object such as {example}")
    return bindings


def keyboard_map():
    """RECOMP_KEYMAP text for private/keyboard.json, e.g. {"a": "Space", "start": "Enter"}; "" if absent."""
    bindings = bindings_file("keyboard.json", '{"a": "Space"}')
    for action, key in bindings.items():
        if action not in KEY_ACTIONS or not isinstance(key, str) or key.upper() not in KEY_CODES:
            raise ValueError(f"private/keyboard.json: cannot bind {action!r} to {key!r}; buttons are "
                             f"{', '.join(KEY_ACTIONS)}; keys are A-Z, 0-9, F1-F9, F12, Up, Down, Left, Right, "
                             "Enter, Backspace, Space, Tab, Shift, Ctrl and Esc")
    return ",".join(f"{action}={KEY_CODES[key.upper()]:x}" for action, key in bindings.items())


def controller_map():
    """RECOMP_PADMAP text for private/controller.json, e.g. {"a": "LB", "white": "A"}; "" if absent."""
    names = {name.upper(): name for name in PAD_NAMES}
    bindings = bindings_file("controller.json", '{"a": "LB"}')
    for action, button in bindings.items():
        if action not in KEY_ACTIONS or not isinstance(button, str) or button.upper() not in names:
            raise ValueError(f"private/controller.json: cannot bind {action!r} to {button!r}; buttons are "
                             f"{', '.join(KEY_ACTIONS)}; controller buttons are {', '.join(PAD_NAMES)}")
    return ",".join(f"{action}={names[button.upper()]}" for action, button in bindings.items())


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
    # Extracted discs name the folder DOA2; match any case so Linux finds it.
    folder = next((entry for entry in disc.iterdir() if entry.name.lower() == "doa2" and entry.is_dir()), None)
    if folder is None:
        return
    for name in HDD_ARCHIVES:
        source, target = folder / name, cache / name
        if not source.is_file() or (target.is_file() and target.stat().st_size == source.stat().st_size):
            continue
        cache.mkdir(parents=True, exist_ok=True)
        partial = target.with_name(name + ".tmp")
        shutil.copyfile(source, partial)
        partial.replace(target)


def screen_shape(text):
    """A "W:H" screen choice as the widescreen value: False for 4:3, else the
    shape, which the runner accepts from 4:3 to 4:1 (recomp-runtime/widescreen.h)."""
    width, colon, height = str(text).partition(":")
    if not (colon and width.isdigit() and height.isdigit() and int(height) > 0
            and 3 * int(width) >= 4 * int(height) and int(width) <= 4 * int(height)
            and int(width) <= 16384 and int(height) <= 16384):
        raise ValueError(f"Screen shape {text!r} must be W:H from 4:3 to 4:1")
    return False if 3 * int(width) == 4 * int(height) else f"{int(width)}:{int(height)}"


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
    for key, option in (("smaa", "smaa"), ("widescreen", "widescreen"), ("vrr", "vrr"), ("skipIntro", "skip_intro"),
                        ("surround", "surround"), ("unlockAll", "unlock_all"), ("vulkan", "vulkan"),
                        ("freeCamera", "free_camera"), ("randomCpuCostumes", "random_cpu_costumes")):
        if isinstance(saved.get(key), bool):
            options[option] = saved[key]
    # Launchers since 16:10 save a "screen" shape instead of the widescreen flag.
    if isinstance(saved.get("screen"), str):
        try:
            options["widescreen"] = screen_shape(saved["screen"])
        except ValueError:
            pass
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
        skip_intro=False, unlock_all=True, surround=None, vrr=None, vulkan=False, free_camera=None,
        random_cpu_costumes=None):
    if seconds is not None and seconds <= 0:
        raise ValueError("Probe duration must be positive")
    if msaa is not None and msaa not in (1, 2, 4, 8, 16, 32):
        raise ValueError("MSAA must be 1, 2, 4, 8, 16 or 32")
    if resolution is not None and resolution not in (480, 720, 1080, 1440, 2160):
        raise ValueError("Resolution must be 480, 720, 1080, 1440 or 2160")
    env = os.environ.copy()
    for name, value in (("RECOMP_D3D_WIDESCREEN", widescreen),
                        ("RECOMP_D3D_MSAA", msaa), ("RECOMP_D3D_SMAA", smaa),
                        ("RECOMP_AUDIO_SURROUND", surround), ("RECOMP_D3D_VRR", vrr),
                        ("RECOMP_FREE_CAMERA", free_camera), ("RECOMP_RANDOM_CPU_COSTUMES", random_cpu_costumes)):
        if value is not None:
            env[name] = value if isinstance(value, str) else str(int(value))
    if resolution is not None:
        env["RECOMP_D3D_SCALE"] = str(resolution / 480)
    keymap = keyboard_map()
    if keymap:
        env.setdefault("RECOMP_KEYMAP", keymap)
    padmap = controller_map()
    if padmap:
        env.setdefault("RECOMP_PADMAP", padmap)
    # The game's Rebind screen (Options, Controller) saves both files here.
    env["RECOMP_KEYBOARD_SETTINGS"] = str(ROOT / "private/keyboard.json")
    env["RECOMP_CONTROLLER_SETTINGS"] = str(ROOT / "private/controller.json")
    # The game's PC SETTINGS page saves the launchers' choices here.
    env["RECOMP_LAUNCHER_SETTINGS"] = str(ROOT / "private/launcher.json")
    # Both presenters keep compiled draw shaders here and load them at the next start.
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
    runner = runner_path(ROOT, vulkan)
    use_vulkan = vulkan and sys.platform == "win32"
    expected = receipt.get("vulkan_runner_sha256" if use_vulkan else "runner_sha256")
    if expected is None:
        raise ValueError("This build has no Vulkan renderer; build the game again" if use_vulkan
                         else "This build has no runner; build the game again")
    if sha256(runner) != expected:
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
    with log_path.open("x", buffering=1, encoding="utf-8", errors="surrogateescape") as log:
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
                with subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      env=env, encoding="utf-8", errors="surrogateescape") as child:
                    copy = threading.Thread(target=lambda: log.writelines(map(redact_private, child.stdout)))
                    copy.start()
                    try:
                        code = child.wait(timeout=seconds)
                    finally:
                        child.kill()  # no-op once it has exited
                        copy.join()
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
    parser.add_argument("--screen", type=screen_shape, metavar="W:H",
                        help="Widescreen in this shape, such as 16:10, 21:9 or 32:9 (4:3 to 4:1); 4:3 turns it off")
    parser.add_argument("--resolution", type=int, choices=(480, 720, 1080, 1440, 2160),
                        help="Render height: 480p, 720p, 1080p, 1440p or 4K; width follows aspect ratio")
    parser.add_argument("--msaa", type=int, choices=(1, 2, 4, 8, 16, 32),
                        help="MSAA samples (1 disables); falls back to GPU support")
    parser.add_argument("--smaa", action=argparse.BooleanOptionalAction, default=None,
                        help="Apply SMAA Ultra after MSAA resolve")
    parser.add_argument("--surround", action=argparse.BooleanOptionalAction, default=None,
                        help="Output 5.1 through the game's own surround mix")
    parser.add_argument("--vrr", action=argparse.BooleanOptionalAction, default=None,
                        help="Present for a G-SYNC or FreeSync display (Direct3D 11)")
    parser.add_argument("--random-cpu-costumes", action=argparse.BooleanOptionalAction, default=None,
                        help="Dress CPU opponents in random unlocked costumes (offline)")
    parser.add_argument("--vulkan", action=argparse.BooleanOptionalAction, default=None,
                        help="Windows: render through Vulkan (SDL_GPU) instead of Direct3D 11")
    parser.add_argument("--free-camera", action=argparse.BooleanOptionalAction, default=None,
                        help="Allow the free camera: F1, ` or a right-stick click takes it (docs/free-camera.md)")
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
                             args.input_analog_pulse_at,
                             args.screen if args.screen is not None else pick("widescreen", args.widescreen),
                             pick("msaa", args.msaa), pick("smaa", args.smaa),
                             pick("resolution", args.resolution), args.story_finale,
                             args.skip_intro or saved.get("skip_intro", False),
                             args.unlock_all and saved.get("unlock_all", True),
                             pick("surround", args.surround), pick("vrr", args.vrr),
                             bool(pick("vulkan", args.vulkan)), pick("free_camera", args.free_camera),
                             pick("random_cpu_costumes", args.random_cpu_costumes)))
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f"Cannot run game: {error}", file=sys.stderr)
        raise SystemExit(1)
