<div align="center">

<img src="https://github.com/NoRain211/doa2u-re/releases/download/readme-media/doa2u-logo.png" alt="Dead or Alive 2 Ultimate logo" width="420">

# DOA2U Native PC Port

**A native Windows, macOS and Linux port of _Dead or Alive 2 Ultimate_ (Xbox), built by static recompilation, with the goal of rewriting it into readable source.**

[![Latest release](https://img.shields.io/github/v/release/NoRain211/doa2u-re?label=alpha)](https://github.com/NoRain211/doa2u-re/releases/latest)
[![CI](https://github.com/NoRain211/doa2u-re/actions/workflows/public-ci.yml/badge.svg)](https://github.com/NoRain211/doa2u-re/actions/workflows/public-ci.yml)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)](LICENSE)
![Platform: Windows | macOS | Linux](https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-0078D6)

[Download](https://github.com/NoRain211/doa2u-re/releases/latest) ·
[Features](#features) ·
[Bring-up notes](docs/bring-up.md) ·
[Report a bug](https://github.com/NoRain211/doa2u-re/issues)

Sister projects: [DOA3](https://github.com/NoRain211/doa3-re) ·
[DOAXBV](https://github.com/NoRain211/doaxbv-re)

[![Support me on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/norainsrecomps)

</div>

> [!IMPORTANT]
> This project ships **no game files**. You need your own legally obtained copy
> of _Dead or Alive Ultimate_ (Xbox, USA), Disc 2. It is not affiliated with or
> endorsed by the game's rights holders.

## About

The DOA2 game executable on Ultimate's second disc is lifted to C with a
static recompiler, then linked against a hand-written runtime that replaces
the Xbox kernel imports and the game's Direct3D 8, DirectSound and input
libraries (XDK build 5849) with native Windows equivalents: Direct3D 11,
XAudio2 and SDL3. On macOS and Linux the same runtime renders through SDL_GPU
(Metal or Vulkan) and plays through SDL3 audio. The rest of the game still runs
as generated code. The plan
is to replace game logic with readable, hand-written source while the game
keeps running.

## Features

- **The whole game, recompiled.** Boot, menus, character select, Story,
  Watch and the attract demo run on the original game code, lifted to C.
- **macOS and Linux.** Native Metal and Vulkan builds through SDL_GPU, for
  Apple Silicon and Intel Macs, Linux PCs and the Steam Deck, from the same
  lifted program and HLSL shaders as Windows. A gamepad settings screen works
  in Steam Game Mode. See [building on macOS and Linux](docs/building.md).
- **Native renderer.** Direct3D 11 at 480p to 4K, the game's own 16:9
  widescreen camera and menus, MSAA up to 32x and SMAA Ultra. Water
  reflections render at the output resolution; bloom and lighting follow the
  game's own passes.
- **Xbox-like pacing.** Frames flip on the next refresh as on the Xbox, at
  60 fps, including heavy rain-stage fights at 4K.
- **Audio and movies.** Music and voices through XAudio2, with 3D voices
  through the game's HRTF tables and I3DL2 reverb, in stereo or the game's
  own 5.1 surround mix. The intro and story movies play.
- **Controllers.** Xbox, PlayStation and other SDL3 controllers, and the
  keyboard for player 1.
- **Launcher.** `Launcher.exe` picks resolution, anti-aliasing, widescreen, an
  optional intro-movie skip and 5.1 surround, and remembers them. `DOA2U.exe` plays
  with the same choices.
- **Saves.** Your profile and unlocks save to `private\play-disc`; every
  character and costume starts unlocked. The game's soft reset
  (Start + Back + both triggers) restarts it.
- **Built from your disc.** `Launcher.exe` lifts the game from your ISO and
  refuses any executable other than the one the developers tested.

## Status

> [!WARNING]
> **0.1.7 Alpha is bring-up software.** Story, Watch and the attract demo play
> in development runs on every stage, but many modes and characters have not
> been tried. A draw the port cannot render yet stops the game with a message
> in the log.

| Area | State |
| --- | --- |
| Boot, menus, attract demo, character select | Working |
| Stages | Every stage played |
| Story endings | Every ending, its credits and epilogue play |
| Story, Ayane and Gen Fu | Stages 1-6 cleared without a stop in scripted runs; the Tengu fight was lost each time |
| Watch | Fights played, including Kasumi vs Lei Fang on The Burai Zenin at 4K and 60 fps |
| Other characters, Versus, Tag, Survival, Time Attack, Sparring | Not yet played through |
| Every character and costume | Unlocked; the game saves the unlocks to your profile |
| Native 16:9 widescreen, up to 4K, MSAA up to 32x, SMAA Ultra | Working, chosen in the launcher |
| Audio, including 3D voices and reverb | Working |
| Intro and story movies | Play; the launcher can skip the intro ones |
| Xbox and other SDL3 controllers, keyboard | Working; keyboard drives player 1 |
| Xbox Live menus | Stop the game; there is no online service |
| Hand-written game logic | Not started; the game runs as generated code |
| macOS and Linux | Boot, menus and the attract demo render at 60 fps through Metal and Vulkan, with MSAA up to 8x and SMAA ([building](docs/building.md)) |

**Known issues:** a demanding moment on The Burai Zenin can still drop a
single frame; the safari stage's heat haze does not ripple yet, and its water
leaves a gap between the elephants' reflections. The game's bloom glows
around bright surfaces as on the Xbox, which reads more strongly on a sharp
4K image.

## Play the alpha

On macOS and Linux, including the Steam Deck, follow
[building on macOS and Linux](docs/building.md) instead.

**Requirements:** Windows 10/11 x64,
[Python 3.12 or newer](https://www.python.org/downloads/windows/) on PATH,
[CMake 3.20 or newer](https://cmake.org/download/) on PATH,
internet access for the first build, and your Disc 2 ISO with about 6 GB free.
The build downloads the pinned [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
compiler (190 MB) into `tools\llvm-mingw` and checks its SHA-256; it needs no
Visual Studio or administrator approval.

1. Download the ZIP from the
   [latest release](https://github.com/NoRain211/doa2u-re/releases/latest) and
   extract it into a new folder with a short path, such as `C:\DOA2U`.
2. Open `Launcher.exe`, drag your Disc 2 ISO onto it (or choose it on the
   **BUILD GAME** page) and choose **Build**. Wait for **Build complete**
   (about 15 minutes). The build stops unless it reproduces the exact program
   the developers tested.
3. Pick resolution, MSAA, SMAA and widescreen on **VIDEO SETTINGS**, stereo or
   5.1 on **AUDIO SETTINGS** and the intro skip on **MOVIES**, then choose
   **START GAME**. A controller, the keyboard or the mouse all work. Your
   choices are remembered, and `DOA2U.exe` plays with them later.

The first launch copies about 700 MB of the game's disc archives into its
hard-disk cache, which takes a few seconds. Saves and the cache
live in `private\play-disc`. The package contains tools and source, never a
prebuilt runner or game data. GitHub's automatic source archives lack the
pinned lifter and extractor; use the release ZIP.

**Controls:** the first connected controller, or the keyboard for player 1:
arrows are the D-pad, Enter is Start, Backspace is Back, Z/X are A/B, A/S are
X/Y, Q/W are White/Black and E/R are the triggers.

## Build from source

The public tests need no game files or lifter checkout:

```powershell
git clone https://github.com/NoRain211/doa2u-re.git
cd doa2u-re
python -m unittest discover -s tools -p "test_*.py"
python tools/public_export.py verify --require-public-tree
cmake -S recomp-runtime -B build/recomp-runtime
cmake --build build/recomp-runtime --config Debug
ctest --test-dir build/recomp-runtime -C Debug --output-on-failure
```

To also test real PCM consumption on this PC's audio endpoint, configure with
`-DRECOMP_NATIVE_AUDIO_TEST=ON`. Passing tests does not establish game
compatibility.

To build the game from a clone, initialize the lifter and pass your ISO:

```powershell
git submodule update --init tools/xboxrecomp
python tools/package_test.py --launchers .
python tools/build_game.py <your-disc-2.iso>
python tools/run_game.py --play
```

The importer extracts the disc once into `private/imported-disc` with XboxDev
[extract-xiso](https://github.com/XboxDev/extract-xiso) (on PATH, or bundled
in the release ZIP), selects the game executable by its SHA-256 and leaves
the ISO unchanged. Only that hash is published, in `tools/build_game.py`.
After a runtime change, `python tools/build_game.py --build-only` reuses the
authenticated lift; `--lift-only` generates without building. Generated C,
receipts and logs stay in `private/`.
The lift reads the committed function list in `tools/doa2u/functions`;
`--disassemble` rebuilds that list from the executable (about 25 minutes)
after a change to `tools/doa2u/seed_functions.json` or the lifter pin.

`tools/doa2u/function_status.json` is the decompilation registry. It records
the Ghidra name for each tracked function and, once it is more than a name,
its `status` (`named`, `decompiled`, `bound` or `tested`). A bound or tested
entry also names the host `source` and `symbol`; the runtime replacement stays
in `recomp-runtime/program_manual.c` or the adapter's own manual lookup. Raw
decompiler output, generated C and retail bytes stay under `private/`.

<!-- function-status-grid:start -->
| Area | Named | Decompiled | Bound / tested | Total | Progress |
| --- | ---: | ---: | ---: | ---: | ---: |
| Game | 157 | 46 | 3 | 206 | 24% |
| CRI middleware | 15 | 0 | 0 | 15 | 0% |
| SDK / CRT | 138 | 131 | 4 | 273 | 49% |
| Untagged | 521 | 131 | 1 | 653 | 20% |
| **Total** | **831** | **308** | **8** | **1,147** | **28%** |
<!-- function-status-grid:end -->

Statuses are exclusive; progress counts decompiled, bound and tested. The
exporter rewrites this table from `function_status.json` after each refresh.

The initial `decompiled` entries are the named functions that already carry a
Ghidra comment; the rest stay `named` until their bodies are worked through.
`python -m unittest discover -s tools -p "test_*.py"` checks every registry
address against the committed function list and checks bound symbols exist.
Refresh names and tags with `tools/doa2u/ghidra/ExportFunctionStatus.py` from
Ghidra's Script Manager against `DOA2.xbe`; add that folder to Ghidra's script
directories or load it through GhidrAssistMCP. It keeps existing statuses and
accepts the registry path as its first argument. Names outside
`tools/doa2u/functions` are skipped and reported; add those addresses to
`seed_functions.json` and regenerate the list first.

`run_game.py` takes the launcher's settings as switches, and
`--seconds <n>` runs a bounded probe instead of `--play`:

```powershell
python tools/run_game.py --play --resolution 2160 --widescreen --msaa 32 --smaa
python tools/run_game.py --play --no-widescreen --msaa 1 --no-smaa
```

The game picks its own 16:9 camera and menu layouts from the dashboard video
setting, and the presenter shows that image at 16:9. MSAA resolves before
SMAA, and gamma applies last; unsupported MSAA counts fall back to the
highest count the GPU supports. Options passed to `run_game.py` bypass the settings
window. The build fetches pinned, hash-verified copies of SDL3 and
[upstream SMAA](https://github.com/iryoku/smaa).

At first launch `run_game.py` builds an image from
`private/dedication-photo.jpg` (shipped in the release ZIP) with Pillow.
`--no-unlock-all` turns off the automatic character and costume unlock, so
unlocks come from play; profiles already saved with the unlock keep it.

To package a release ZIP from a clean checkout:

```powershell
python tools/package_test.py 0.1.7-Alpha
python tools/package_test.py 0.1.7-Alpha --platform macos
python tools/package_test.py 0.1.7-Alpha --platform linux
```

It zips committed HEAD, the pinned lifter and the photo from
`private/release-assets/`. The Windows ZIP adds extract-xiso and its license
with [test-release.md](docs/test-release.md) as `README.txt`; the macOS and
Linux ZIPs use [test-release-macos.md](docs/test-release-macos.md) and
[test-release-linux.md](docs/test-release-linux.md).

## Repository layout

| Path | Contents |
| --- | --- |
| [`recomp-runtime/`](recomp-runtime) | Runtime, kernel and input adapters, audio, D3D8 replacements, D3D11 and SDL_GPU presenters, tests |
| [`xbe/`](xbe) | XBE parsing and hashing |
| [`tools/`](tools) | Disc import, build, launcher, packaging and export checks; pinned lifter submodule |
| [`docs/`](docs) | Bring-up notes, reuse provenance, decisions and the release README |
| `private/` | Ignored local game inputs, generated output and run evidence |

## Contributing

Translation defects are fixed in the
[lifter](https://github.com/NoRain211/xboxrecomp) and regenerated; generated
game C is never patched by hand. Read [bring-up](docs/bring-up.md) and
[reuse](docs/reuse.md) before changing an adapter.

When [reporting a bug](https://github.com/NoRain211/doa2u-re/issues), say
what you did and what happened, and copy the `recomp stop` or crash lines
from the end of the newest log in `private\play-logs` (or the error from the
newest `private\build-logs` folder). Remove private paths, and never upload
game binaries, generated game C, assets, extracted filenames, saves or whole
logs.

## AI assistance

This project uses large language models and AI coding agents extensively for
code, reverse-engineering analysis, debugging, tests and documentation. Their
output can contain mistakes; changes are reviewed and tested, and unverified
behavior is tracked as such.

## License

Original code and documentation are licensed under
[GPL-3.0-or-later](LICENSE); see also [NOTICE](NOTICE). Third-party components
keep their own licenses. The license grants no rights to the game or its assets.
