#!/usr/bin/env bash
# prewarm_shaders.sh — Phase 27C: Compile base shaders into DDC offline.
# Reduces cold-start from ~180s to ~60s by pre-populating the Derived Data Cache.
# Run once after a fresh checkout or engine upgrade.
#
# Usage: ./scripts/prewarm_shaders.sh [--editor-path /path/to/UE5Editor]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROJECT_FILE="${PROJECT_ROOT}/unreal_project/CamSimTest/CamSimTest.uproject"

# Default UE5 editor path — override with --editor-path or UE5_EDITOR env var
UE5_EDITOR="${UE5_EDITOR:-/opt/UnrealEngine/Engine/Binaries/Linux/UnrealEditor}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --editor-path) UE5_EDITOR="$2"; shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

if [[ ! -x "${UE5_EDITOR}" ]]; then
    echo "ERROR: UE5 editor not found at: ${UE5_EDITOR}"
    echo "Set UE5_EDITOR env var or pass --editor-path /path/to/UnrealEditor"
    exit 1
fi

echo "[prewarm_shaders] Starting DDC pre-warm for: ${PROJECT_FILE}"
echo "[prewarm_shaders] Editor: ${UE5_EDITOR}"
START_TIME=$(date +%s)

# Run editor in commandlet mode to populate DDC.
# -run=DerivedDataCache: runs the DDC fill commandlet
# -fill: fills the DDC with all shaders referenced by the project
# -stdout: stream log to stdout
# -unattended: no interactive dialogs
# -nopause: don't pause on exit
"${UE5_EDITOR}" "${PROJECT_FILE}" \
    -run=DerivedDataCache \
    -fill \
    -stdout \
    -unattended \
    -nopause \
    -nosound \
    -nullrhi 2>&1 | grep -E "(DDC|Shader|Warning|Error|LogInit)" || true

END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))
echo "[prewarm_shaders] DDC pre-warm completed in ${ELAPSED}s"
echo "[prewarm_shaders] Re-run when: engine upgraded, new materials added, or first checkout"
