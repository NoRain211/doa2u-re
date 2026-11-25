#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Installs the packages build_game.sh needs on Linux:
#   ./install_deps.sh [--print]     (--print shows the command without running it)
set -euo pipefail
if [[ -r /etc/os-release ]] && grep -qx 'ID=steamos' /etc/os-release; then
    echo "SteamOS is read-only with no compiler. Build inside an Ubuntu 24.04 distrobox," >&2
    echo "run this script there, and see 'Linux and SteamOS' in docs/building.md." >&2
    exit 1
fi
if command -v apt-get >/dev/null; then
    cmd=(apt-get install -y --no-install-recommends build-essential clang cmake ninja-build git python3-venv
         ca-certificates
         libshaderc1 libvulkan1 mesa-vulkan-drivers
         libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev
         libxkbcommon-dev libwayland-dev libegl-dev libasound2-dev libpulse-dev
         libpipewire-0.3-dev libudev-dev libdbus-1-dev libxtst-dev)
elif command -v dnf >/dev/null; then
    cmd=(dnf install -y gcc-c++ clang cmake ninja-build git python3
         libshaderc vulkan-loader mesa-vulkan-drivers
         libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel libXScrnSaver-devel
         libxkbcommon-devel wayland-devel wayland-protocols-devel mesa-libEGL-devel alsa-lib-devel
         pulseaudio-libs-devel pipewire-devel systemd-devel dbus-devel libXtst-devel)
elif command -v pacman >/dev/null; then
    cmd=(pacman -S --needed --noconfirm base-devel clang cmake ninja git python
         shaderc vulkan-icd-loader mesa
         libx11 libxext libxrandr libxcursor libxi libxss libxkbcommon wayland wayland-protocols
         alsa-lib libpulse pipewire dbus libxtst)
else
    echo "No apt, dnf or pacman found. Install the packages listed in docs/building.md by hand." >&2
    exit 1
fi
# NVIDIA GPUs get Vulkan from their own driver package; mesa covers AMD and Intel.
if [[ ${EUID} -ne 0 ]]; then cmd=(sudo "${cmd[@]}"); fi
if [[ "${1:-}" == "--print" ]]; then
    echo "${cmd[*]}"
else
    "${cmd[@]}"
fi
