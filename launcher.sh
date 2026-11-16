#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# macOS and Linux counterpart of Launcher.exe: pick resolution,
# anti-aliasing, widescreen, the intro skip, 5.1 surround and the staff-roll
# dedication card (saved to private/launcher.json), then play with those choices.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# The Linux package ships a prebuilt launcher (tools/build_linux_launcher.sh), so its BUILD GAME
# page works before any build. A checkout has none and uses the one the game build makes.
if [[ "$(uname -s)" != "Darwin" ]]; then
    PREBUILT="${ROOT}/tools/artifacts/launcher-linux-x86_64"
    if [[ -x "${PREBUILT}" ]]; then
        if "${PREBUILT}" "${ROOT}"; then
            exec "${ROOT}/run_game.sh" --play --saved "$@"
        fi
        exit 1
    fi
    exec "${ROOT}/run_game.sh" --play --launcher "$@"
fi

SOURCE="${ROOT}/tools/launcher_macos.swift"
BINARY="${ROOT}/private/launcher_macos"

# Built on first use (and after edits) with the Xcode command line tools.
if [[ ! -x "${BINARY}" || "${SOURCE}" -nt "${BINARY}" ]]; then
    if ! command -v swiftc >/dev/null 2>&1; then
        echo "The launcher needs the Xcode command line tools: xcode-select --install" >&2
        echo "Or edit private/launcher.json and run ./run_game.sh directly." >&2
        exit 1
    fi
    mkdir -p "${ROOT}/private"
    swiftc -O "${SOURCE}" -o "${BINARY}"
fi

if "${BINARY}" "${ROOT}"; then
    exec "${ROOT}/run_game.sh" --play --saved "$@"
fi
