#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# macOS and Linux counterpart of the Build tab in Launcher.exe:
#   ./build_game.sh /path/to/disc2.iso [--extractor /path/to/extract-xiso]
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON="python3"
VENV="${SCRIPT_DIR}/private/venv"
if [[ -f "${VENV}/bin/python3" ]]; then
    PYTHON="${VENV}/bin/python3"
fi
if ! "${PYTHON}" -c "import sys; sys.exit(sys.version_info < (3, 12))" 2>/dev/null; then
    echo "Python 3.12 or newer is required; see docs/building.md." >&2
    exit 1
fi
# Debian and Ubuntu refuse a global pip install, so the build keeps its own packages.
if [[ "${PYTHON}" == "python3" ]]; then
    if ! "${PYTHON}" -m venv "${VENV}"; then
        rm -rf "${VENV}"
        echo "Could not create ${VENV}. On Debian or Ubuntu: sudo apt install python3-venv" >&2
        exit 1
    fi
    PYTHON="${VENV}/bin/python3"
fi
# Check now rather than after the disc import; --lift-only builds nothing.
if [[ " $* " != *" --lift-only "* ]]; then
    missing=()
    for tool in cmake ninja git c++; do
        command -v "${tool}" >/dev/null || missing+=("${tool}")
    done
    if (( ${#missing[@]} )); then
        echo "Missing build tools: ${missing[*]}. See the apt line in docs/building.md"              "(Debian/Ubuntu: sudo apt install build-essential cmake ninja-build git)." >&2
        exit 1
    fi
fi
exec "${PYTHON}" "${SCRIPT_DIR}/tools/build_game.py" "$@"
