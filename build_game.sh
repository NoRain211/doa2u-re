#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# macOS and Linux counterpart of the Build tab in Launcher.exe:
#   ./build_game.sh /path/to/disc2.iso [--extractor /path/to/extract-xiso]
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON="python3"
if [[ -f "${SCRIPT_DIR}/private/venv/bin/python3" ]]; then
    PYTHON="${SCRIPT_DIR}/private/venv/bin/python3"
fi
if ! "${PYTHON}" -c "import sys; sys.exit(sys.version_info < (3, 12))" 2>/dev/null; then
    echo "Python 3.12 or newer is required; see docs/building.md." >&2
    exit 1
fi
exec "${PYTHON}" "${SCRIPT_DIR}/tools/build_game.py" "$@"
