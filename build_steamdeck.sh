#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Steam Deck (SteamOS) build in one step: ./build_steamdeck.sh /path/to/disc2.iso
# SteamOS has no compiler, so this builds inside an Ubuntu 24.04 distrobox that
# shares your home folder, installing distrobox and the build packages first.
# The finished checkout runs on SteamOS itself (./launcher.sh or ./run_game.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONTAINER="doa2u-build"
DISTROBOX_VERSION="1.8.2.5"
DISTROBOX_SHA256="0c3bc4785ee3be3b89f93abb7cc0a9f60e56989e81319af140a4b60403b18f80"

if [[ "${1:-}" == "--inside" ]]; then
    shift
    cd "${ROOT}"
    sudo apt-get update
    ./install_deps.sh
    exec ./build_game.sh "$@"
fi

if [[ "$#" -eq 0 ]]; then
    echo "Usage: ./build_steamdeck.sh /path/to/disc2.iso" >&2
    exit 1
fi
# The container shares only the home folder, so the checkout has to live there.
if [[ "${ROOT}" != "${HOME}"/* ]]; then
    echo "Move this folder under ${HOME} first; the build container shares only your home folder." >&2
    exit 1
fi

export PATH="${HOME}/.local/bin:${PATH}"
if ! command -v distrobox >/dev/null 2>&1; then
    echo "Installing distrobox ${DISTROBOX_VERSION} into ~/.local"
    command -v podman >/dev/null 2>&1 || { echo "SteamOS should include podman; it was not found." >&2; exit 1; }
    tmp="$(mktemp -d)"
    trap 'rm -rf "${tmp}"' EXIT
    curl -fL -o "${tmp}/distrobox.tar.gz" \
        "https://github.com/89luca89/distrobox/archive/refs/tags/${DISTROBOX_VERSION}.tar.gz"
    echo "${DISTROBOX_SHA256}  ${tmp}/distrobox.tar.gz" | sha256sum -c -
    tar xzf "${tmp}/distrobox.tar.gz" -C "${tmp}"
    "${tmp}/distrobox-${DISTROBOX_VERSION}/install" --prefix "${HOME}/.local"
fi

if ! distrobox list | grep -q "[| ]${CONTAINER} "; then
    distrobox create --yes --name "${CONTAINER}" --image ubuntu:24.04
fi
# An ISO outside the home folder is not visible in the container.
exec distrobox enter "${CONTAINER}" -- "${ROOT}/build_steamdeck.sh" --inside "$@"
