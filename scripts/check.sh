#!/usr/bin/env bash
# check.sh — Compile-check CamSimTestEditor and show only actionable output.
#
# Usage:
#   ./scripts/check.sh [--verbose]
#
# Options:
#   --verbose    Skip filter; show raw UBT output
#
# Exit code mirrors UBT: 0 = success, non-zero = build failed.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UE_PROJECT="${REPO_ROOT}/unreal_project/CamSimTest/CamSimTest.uproject"

VERBOSE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --verbose|-v) VERBOSE=1; shift ;;
        --help|-h)
            sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) echo "[ERROR] Unknown argument: $1"; exit 1 ;;
    esac
done

# -------------------------------------------------------------------------
# Platform detection
# -------------------------------------------------------------------------
OS="$(uname -s)"
case "${OS}" in
    Darwin) PLATFORM="mac"   ;;
    Linux)  PLATFORM="linux" ;;
    *)
        echo "[ERROR] Unsupported platform: ${OS}"
        exit 1
        ;;
esac

# -------------------------------------------------------------------------
# Locate UE / UBT
# -------------------------------------------------------------------------
UE_BINARY="${UE_BINARY:-}"

if [ "${PLATFORM}" = "mac" ]; then
    UE_SEARCH_PATHS=(
        "/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
        "/Applications/UnrealEditor.app/Contents/MacOS/UnrealEditor"
    )
    for CANDIDATE in "${UE_SEARCH_PATHS[@]}"; do
        [ -f "${CANDIDATE}" ] && { UE_BINARY="${CANDIDATE}"; break; }
    done
    if [ -z "${UE_BINARY}" ]; then
        UE_BINARY="$(mdfind 'kMDItemFSName == "UnrealEditor"' 2>/dev/null \
            | grep -i 'UE_5.7.*MacOS/UnrealEditor$' | head -1 || true)"
    fi
else
    UE_SEARCH_PATHS=(
        "${HOME}/UnrealEngine/Engine/Binaries/Linux/UnrealEditor"
        "/opt/UnrealEngine/Engine/Binaries/Linux/UnrealEditor"
        "/opt/Epic/UE_5.7/Engine/Binaries/Linux/UnrealEditor"
        "${HOME}/.local/share/UnrealEngine/Engine/Binaries/Linux/UnrealEditor"
    )
    for CANDIDATE in "${UE_SEARCH_PATHS[@]}"; do
        [ -f "${CANDIDATE}" ] && { UE_BINARY="${CANDIDATE}"; break; }
    done
    if [ -z "${UE_BINARY}" ]; then
        UE_BINARY="$(find /opt "${HOME}" -maxdepth 6 -name UnrealEditor \
            -path '*/Binaries/Linux/UnrealEditor' 2>/dev/null | head -1 || true)"
    fi
fi

if [ -z "${UE_BINARY}" ]; then
    echo "[ERROR] UnrealEditor not found. Install UE 5.7 or set UE_BINARY."
    exit 1
fi

UE_ROOT="${UE_BINARY%%/Engine/Binaries/*}"
if [ "${PLATFORM}" = "mac" ]; then
    UBT="${UE_ROOT}/Engine/Build/BatchFiles/Mac/Build.sh"
    UBT_PLATFORM="Mac"
else
    UBT="${UE_ROOT}/Engine/Build/BatchFiles/Linux/Build.sh"
    UBT_PLATFORM="Linux"
fi

if [ ! -f "${UBT}" ]; then
    echo "[ERROR] Build.sh not found: ${UBT}"
    exit 1
fi

# -------------------------------------------------------------------------
# DDC env (keep UBT output quiet by pointing it at the repo-local cache)
# -------------------------------------------------------------------------
DDC_LOCAL_ROOT="${CAMSIM_DDC_DIR:-${REPO_ROOT}/.cache/ue-ddc}"
DDC_ZEN_ROOT="${DDC_LOCAL_ROOT}/Zen"
mkdir -p "${DDC_LOCAL_ROOT}" "${DDC_ZEN_ROOT}"

DDC_ENV=(
    "UE-LocalDataCachePath=${DDC_LOCAL_ROOT}"
    "UE_LocalDataCachePath=${DDC_LOCAL_ROOT}"
    "UE-SharedDataCachePath=None"
    "UE_SharedDataCachePath=None"
    "UE-ZenDataPath=${DDC_ZEN_ROOT}"
    "UE_ZenDataPath=${DDC_ZEN_ROOT}"
    "UE-ZenSubprocessDataPath=${DDC_ZEN_ROOT}"
    "UE_ZenSubprocessDataPath=${DDC_ZEN_ROOT}"
)

# -------------------------------------------------------------------------
# Run UBT
# -------------------------------------------------------------------------
echo "==> Checking CamSimTestEditor (${UBT_PLATFORM} Development) …"

if [ "${VERBOSE}" -eq 1 ]; then
    env "${DDC_ENV[@]}" "${UBT}" CamSimTestEditor "${UBT_PLATFORM}" Development "${UE_PROJECT}" -waitmutex
    UBT_EXIT=$?
else
    # Capture UBT output; filter to actionable lines only.
    # Use a temp file so we can get the real exit code despite the pipe.
    TMP_OUT="$(mktemp)"
    set +e
    env "${DDC_ENV[@]}" "${UBT}" CamSimTestEditor "${UBT_PLATFORM}" Development "${UE_PROJECT}" -waitmutex \
        > "${TMP_OUT}" 2>&1
    UBT_EXIT=$?
    set -e

    FILTERED="$(grep -E \
        'error:|Error :|warning:|Warning :|fatal error:|undefined reference|Source/CamSimTest/' \
        "${TMP_OUT}" \
        | grep -vE \
        '^\[|^LogDDC|^Log[A-Z]|^@progress|^UnrealBuildTool:|^\s*$' \
        || true)"

    if [ -n "${FILTERED}" ]; then
        echo "${FILTERED}"
    fi

    # Count errors and warnings in filtered output
    ERR_COUNT="$(echo "${FILTERED}" | grep -cE 'error:|Error :|fatal error:|undefined reference' || true)"
    WARN_COUNT="$(echo "${FILTERED}" | grep -cE 'warning:|Warning :' || true)"

    rm -f "${TMP_OUT}"

    echo ""
    if [ "${UBT_EXIT}" -eq 0 ]; then
        if [ "${WARN_COUNT}" -gt 0 ]; then
            echo "Build succeeded — ${WARN_COUNT} warning(s)"
        else
            echo "Build succeeded"
        fi
    else
        echo "Build FAILED — ${ERR_COUNT} error(s), ${WARN_COUNT} warning(s)"
    fi
fi

exit "${UBT_EXIT}"
