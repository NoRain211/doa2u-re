# Building on macOS and Linux

The runner also builds natively for macOS (Apple Silicon or Intel) and Linux
x86-64, including SteamOS. It is the same lifted program and runtime as the
Windows build; only the host layer differs:

| | Windows | macOS | Linux |
|---|---|---|---|
| Rendering | Direct3D 11 | Metal through SDL_GPU | Vulkan through SDL_GPU |
| Draw shaders | HLSL, D3DCompile | the same HLSL, shaderc and SPIRV-Cross | the same HLSL, shaderc |
| Audio | XAudio2 | SDL3 (CoreAudio) | SDL3 (PipeWire, PulseAudio or ALSA) |
| Input | SDL3 gamepads, keyboard | same | same |
| Fibers and threads | Win32 | `win32_compat.c` and `fiber_switch_*.S` | same |

Both presenters build their draw shaders from one HLSL source
(`recomp-runtime/d3d_draw_shader.cpp`), so a shading fix reaches every
platform. The macOS and Linux host layer comes from the sister projects
[doaxbv-re](https://github.com/NoRain211/doaxbv-re) and
[doa3-re](https://github.com/NoRain211/doa3-re).

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
`private/imported-disc`, about 4.1 GB.

`./launcher.sh` opens a window to pick resolution, MSAA, SMAA, widescreen,
the intro skip and 5.1 surround, saves them to `private/launcher.json` and starts the game.
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
A fresh container has none of these. On Ubuntu 24.04 or Debian 13:

```bash
sudo apt install build-essential cmake ninja-build git python3-venv \
    libshaderc1 libvulkan1 mesa-vulkan-drivers \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev \
    libxkbcommon-dev libwayland-dev libegl-dev libasound2-dev libpulse-dev \
    libpipewire-0.3-dev libudev-dev libdbus-1-dev
```

When no `extract-xiso` is on `PATH`, the build downloads upstream's pinned
Linux release into `private/` and checks its SHA-256. That binary needs
glibc 2.34 or newer; on other systems, build `extract-xiso` as in the macOS
section and pass it with `--extractor`. `build_game.sh` builds SDL3 when the
distribution has none:

```bash
python3 -m venv private/venv
private/venv/bin/pip install capstone==5.0.7
./build_game.sh /path/to/disc2.iso
```

SteamOS has a read-only system and no compiler, so build inside a container
such as an Ubuntu 24.04 `distrobox` that shares your home folder (Bazzite
includes distrobox); the finished checkout then runs on SteamOS itself:

```bash
distrobox create --name doa2u --image ubuntu:24.04
distrobox enter doa2u
```

Run the `apt install` above inside it. `./launcher.sh` opens a
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
- `RECOMP_AUDIO_CAPTURE` records SDL's dry mix as a float WAV and does not
  stop after 60 seconds.
- `RECOMP_HEADLESS=1` hides the window, for scripted runs.
- On ARM64 hosts `RCPSS` and `RSQRTSS` return the exact reciprocal instead of
  Intel's table approximation. The result is within Intel's documented
  error, but not bit-identical. x86-64 hosts use the native instructions.

## Runtime tests

```bash
cmake -S recomp-runtime -B build/recomp-runtime -G Ninja
cmake --build build/recomp-runtime
ctest --test-dir build/recomp-runtime --output-on-failure
```

These build test executables, not the game runner, and need no game files.
The presenter test renders on the GPU; on a Linux machine without one, set
`SDL_VIDEO_DRIVER=offscreen` and install Mesa's lavapipe.
