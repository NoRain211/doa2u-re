#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# macOS and Linux counterpart of DOA2U.exe. Without arguments it plays with the
# choices launcher.sh saved; arguments go to tools/run_game.py.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON="python3"
if [[ -f "${SCRIPT_DIR}/private/venv/bin/python3" ]]; then
    PYTHON="${SCRIPT_DIR}/private/venv/bin/python3"
fi
if [[ $# -eq 0 ]]; then
    set -- --play --saved
fi
exec "${PYTHON}" "${SCRIPT_DIR}/tools/run_game.py" "$@"
