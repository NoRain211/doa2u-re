# Building on macOS and Linux

The runner also builds natively for macOS (Apple Silicon or Intel) and Linux
x86-64, including SteamOS. It is the same lifted program and runtime as the
Windows build; only the host layer differs:

| | Windows | macOS | Linux |
|---|---|---|---|
| Rendering | Direct3D 11, or Vulkan through SDL_GPU | Metal through SDL_GPU | Vulkan through SDL_GPU |
| Draw shaders | HLSL, D3DCompile (Vulkan: shaderc) | the same HLSL, shaderc and SPIRV-Cross | the same HLSL, shaderc |
| Audio | XAudio2 | SDL3 (CoreAudio) | SDL3 (PipeWire, PulseAudio or ALSA) |
| Input | SDL3 gamepads, keyboard | same | same |
| Fibers and threads | Win32 | `win32_compat.c` and `fiber_switch_*.S` | same |

Both presenters build their draw shaders from one HLSL source
(`recomp-runtime/d3d_draw_shader.cpp`), so a shading fix reaches every
platform. Windows builds a second runner, `recomp_program_runner_vulkan.exe`,
with the SDL_GPU presenter; the launcher's Renderer setting picks it, and the
build fetches LWJGL's `shaderc.dll` for it. The rendering differences listed
below apply to it too; it keeps XAudio2. The macOS and Linux host layer comes
from the sister projects
[doaxbv-re](https://github.com/NoRain211/doaxbv-re) and
[doa3-re](https://github.com/NoRain211/doa3-re).

## Build from the launcher

`./launcher.sh` opens on its BUILD GAME page while the game is not built. Choose
your Disc 2 ISO with DISC ISO, or drop it on the window, and choose BUILD. The
page shows the elapsed time and the build's latest line, and keeps the full
output in `private/build.log`; leaving while it runs asks twice, then stops it.
When it finishes, choose START GAME.

The Linux package ships a prebuilt launcher, so this page needs nothing
installed. The build it starts still needs the tools below; on SteamOS it runs
`build_steamdeck.sh`, which sets up its own container. On macOS the launcher
compiles on first use and needs the Xcode command line tools.

## macOS

Install CMake, Ninja, SDL3, the shader tools and Python 3.12 or newer:

```bash
brew install cmake ninja sdl3 shaderc spirv-cross python@3.13
```

Homebrew has no `extract-xiso`. Build it from
[XboxDev/extract-xiso](https://github.com/XboxDev/extract-xiso) and put it on
`PATH`, or pass its location with `--extractor`:

```bash
git clone https://github.com/XboxDev/extract-xiso.git
cmake -S extract-xiso -B extract-xiso/build && cmake --build extract-xiso/build
```

The lifter needs Capstone 5.0.7. Homebrew's Python refuses a global
`pip install`, so put it in the virtual environment the scripts look for:

```bash
git clone --recurse-submodules https://github.com/NoRain211/doa2u-re.git
cd doa2u-re
python3.13 -m venv private/venv
private/venv/bin/pip install capstone==5.0.7
```

Then build from your Disc 2 ISO:

```bash
./build_game.sh /path/to/disc2.iso --extractor extract-xiso/build/extract-xiso
```

Lifting takes several minutes. The generated program is checked against the
same manifest hash as the Windows build, so a different lifter or input stops
the build before it compiles. The ISO is extracted once into
`private/imported-disc`, about 4.1 GB, and moved to `private/play-disc`
once the build succeeds.

`./launcher.sh` opens the same menu as the Windows launcher: resolution, MSAA,
SMAA and widescreen on **VIDEO SETTINGS**, stereo or 5.1 on **AUDIO SETTINGS**
and the intro skip on **MOVIES**. **START GAME** saves them to
`private/launcher.json` and starts the game. A
`private/launcher-background.png` (or `.jpg` on macOS) replaces its backdrop.
`./run_game.sh` starts the game with the saved choices, and
`./run_game.sh --play --resolution 1080 --msaa 4 --widescreen` with explicit
ones. As on Windows, the game keeps its cache and saves in
`private/play-disc` and each session's log in `private/play-logs`.

## Linux and SteamOS

Linux renders with Vulkan and compiles shaders at run time with shaderc, so
the host needs `libshaderc_shared.so.1` (SteamOS includes it) or, on Debian
and Ubuntu, `libshaderc.so.1` from the `libshaderc1` package.

Build in an x86-64 distribution with CMake, Ninja, a C++17 compiler, Git,
Python 3.12 or newer with the `venv` module and SDL3's build dependencies.
A fresh container has none of these. `./install_deps.sh` installs them with
apt, dnf or pacman (`--print` only shows the command). On Ubuntu 24.04 or
Debian 13 the same packages are:

```bash
sudo apt install build-essential cmake ninja-build git python3-venv \
    libshaderc1 libvulkan1 mesa-vulkan-drivers \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev \
    libxkbcommon-dev libwayland-dev libegl-dev libasound2-dev libpulse-dev \
    libpipewire-0.3-dev libudev-dev libdbus-1-dev libxtst-dev
```

When no `extract-xiso` is on `PATH`, the build downloads upstream's pinned
Linux release into `private/` and checks its SHA-256. That binary needs
glibc 2.34 or newer; on other systems, build `extract-xiso` as in the macOS
section and pass it with `--extractor`. `build_game.sh` builds SDL3 when the
distribution has none:

Run these from inside the clone. `build_game.sh` sits at its top level, so
`No such file or directory` means you are in the wrong folder (`ls` should list
it). Quote the ISO path if it contains spaces:

```bash
git clone --recurse-submodules https://github.com/NoRain211/doa2u-re.git
cd doa2u-re
./build_game.sh "/path/to/disc2.iso"
```

`build_game.sh` creates `private/venv` and installs Capstone there itself, and
stops at once if CMake, Ninja, Git or a C++ compiler is missing.

SteamOS has a read-only system and no compiler. On a Steam Deck, keep the
checkout under your home folder, open Konsole in Desktop Mode and run:

```bash
./build_steamdeck.sh "/path/to/disc2.iso"
```

It installs distrobox into `~/.local` when it is missing, creates an Ubuntu
24.04 container named `doa2u-build` that shares your home folder, runs
`./install_deps.sh` there and then `./build_game.sh`. The ISO must be under
your home folder too. The finished checkout runs on SteamOS itself. To do it
by hand, create a container with `distrobox create --name doa2u --image
ubuntu:24.04`, enter it and run `./install_deps.sh` and `./build_game.sh`.

`./launcher.sh` opens a
gamepad-friendly settings screen, then starts the game; `./run_game.sh`
starts the game with the saved choices.

For Steam Game Mode, add `launcher.sh` (or `run_game.sh`) with **Add a
Non-Steam Game** in Desktop Mode. Under Steam the game and launcher open full
screen. If one controller drives two player ports, turn Steam Input off for
that shortcut.

## Differences from the Windows build

- MSAA stops at the 8x SDL_GPU supports (Apple GPUs at 4x); a higher choice
  falls back as on Windows. SMAA runs the same upstream shader.
- SDL_GPU has no geometry shaders or occlusion queries. Wide and antialiased
  lines are expanded on the CPU, a draw's coverage probe rasterizes on the
  CPU, and the glare fade's visibility tests count passing samples with an
  additive pass. The presenter tests check all three against the Direct3D 11
  results.
- Metal samplers have no LOD bias, so on macOS the shader adds each
  sampler's bias. Vulkan applies it in the sampler, as Direct3D 11 does.
- Depth is 32-bit float with 8-bit stencil, as Apple and AMD GPUs lack D24S8.
- The I3DL2 reverb is FAudio's reimplementation of XAudio2's reverb
  (`recomp-runtime/audio_reverb.cpp`).
- `RECOMP_D3D_VRR` and `RECOMP_D3D_PRESENT_LOG` are Direct3D 11 features.
- `RECOMP_D3D_FRAMEGEN=1` is an experimental option of the Windows runners,
  Direct3D 11 and Vulkan. The **Frame rate: Display** row on the Windows
  launcher's (`Launcher.exe`) VIDEO SETTINGS page and in the game's PC
  SETTINGS sets it at the next launch. Both save the choice, so when the game
  starts through the launcher or `run_game.py`, the saved Frame rate sets the
  variable and takes priority over one you set by hand. On a display
  of 120 Hz, 240 Hz or another whole multiple of 60 Hz,
  it presents k = refresh / 60 frames per guest frame on VSYNC: k - 1
  generated frames between the last two real frames, then the newer real
  frame, so the picture is up to one guest frame later. The presenter
  estimates the motion between the last two real frames by block matching and
  moves both along it into each generated frame. Where the motion is doubtful
  (flat or noisy areas such as water, repeating patterns, the edges of moving
  objects), it blends toward a crossfade of the two, so moving objects ghost
  there. The game's 2D layer (life bars, timer, names, menus and logos, but
  not fades or backdrops that span the screen's width) does not move: it
  shows the nearer real frame, and the scene is not warped from it. At a
  scene cut, where most of the picture's shapes change between the two real
  frames, the generated frames show the older real frame, so the cut lands
  with the newer one as at 60 Hz; fades and flashes keep the shapes and
  crossfade, and a very fast camera move can be taken for one cut. After frame generation was off, a quality change, or a real
  frame more than 1.5 guest frames late (a load; 2.5 on Direct3D 11, where
  the presenter's own work sits in that gap), the generated frames start
  again from the newer real frame.
  `RECOMP_D3D_FRAMEGEN=flow` shows the motion instead, as color per search
  block: hue is the direction, brightness the speed, dark blocks have low
  confidence, the 2D layer is whitened, and all of it turns red at a cut.
  The presenter worker presents each one when it is due, also between the
  draws of the next real frame, and skips one it would show late; real
  frames are never delayed. Any other
  refresh, and macOS and Linux, log `framegen unsupported` and run as before.
  It is off while the window is hidden or minimized, in an unpaced or
  fast-forwarded run and under the frame clock (netplay and synctest).
  Direct3D 11 also leaves it off with `RECOMP_D3D_VRR=1`, which logs
  `framegen unsupported: VRR is on`: the two are not combined yet. With
  `RECOMP_PERF_COUNTER=1` it logs a `recomp framegen` line each second.
- `RECOMP_D3D_FRAMEGEN_CAPTURE=<folder>`, with frame generation on in the
  Vulkan renderer, writes what it gets for each real frame into the folder:
  the picture before gamma, the HUD mask, the gamma ramp, and whether the
  frame started the history again. `RECOMP_D3D_FRAMEGEN_CAPTURE_AT` (first
  present, default 1)
  and `_COUNT` (default all) select the frames. Frame generation then also
  runs with `RECOMP_HEADLESS=1`. The game runs slowly while it writes, and a
  2160 frame takes 41 MB; keep the quality fixed during a capture. The frames
  are game output: keep captures under `private/`. Built with the runtime
  tests, `recomp_d3d_framegen_replay <capture> <output>` renders every frame
  the window would show at k times 60 Hz (`--k 4` by default): for each real
  frame, its k - 1 generated frames, then the frame, after gamma, through the
  presenter's own frame generation, as `<frame>-<slot>.bmp` files.
  `--crossfade` replaces the generated frames with the plain crossfade and
  `--original` repeats each real frame k times, as the game looks at 60 Hz.
  `--ffmpeg` and the options after it pipe the frames to `ffmpeg` on `PATH`
  instead, with `<output>` as its output file, for example
  `--ffmpeg -c:v ffv1`. Those options and `<output>` go through `cmd.exe`,
  so the tool refuses any that hold `"`, `%`, `^`, `&`, `|`, `<`, `>` or a
  line break. The first real frame starts the history. Direct3D 11 does not
  capture; it logs that capture is unsupported.
- `RECOMP_AUDIO_CAPTURE` records SDL's dry mix as a float WAV and does not
  stop after 60 seconds.
- `RECOMP_HEADLESS=1` hides the window, for scripted runs.

## Runtime tests

```bash
cmake -S recomp-runtime -B build/recomp-runtime -G Ninja
cmake --build build/recomp-runtime
ctest --test-dir build/recomp-runtime --output-on-failure
```

These build test executables, not the game runner, and need no game files.
The presenter test renders on the GPU; on a Linux machine without one, set
`SDL_VIDEO_DRIVER=offscreen` and install Mesa's lavapipe.
