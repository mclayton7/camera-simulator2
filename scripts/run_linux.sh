#!/usr/bin/env bash
# run_linux.sh
#
# Build (optional) and launch CamSim on Linux for development and testing.
#
# Looks for the binary in the following locations (in order):
#   1. Packaged Shipping build:  Saved/StagedBuilds/Linux/CamSimTest
#   2. UnrealEditor (PIE -game): system-wide or user UE5.7 installation
#
# Usage:
#   ./scripts/run_linux.sh [options]
#
# Options:
#   --build             Compile C++ with UnrealBuildTool before launching
#   --build-only        Compile C++ and exit without launching
#   --mode editor       Run in UnrealEditor -game (default when no package exists)
#   --mode packaged     Force packaged shipping binary
#   --headless          Add -RenderOffScreen (no window, useful for CI)
#   --local             Output to 127.0.0.1:5004 (unicast — recommended for dev)
#   --multicast ADDR    Override output address (default: 239.1.1.1)
#   --mcast-port PORT   Override output UDP port (default: 5004)
#   --cigi-port PORT    Override CIGI listen port (default: 8888)
#   --log               Tail the UE log after launch, filtered to LogCamSim
#   --help              Show this message
#
# Typical dev workflow:
#   ./scripts/run_linux.sh --build --local --log
#
# Receiving the stream:
#   Unicast (--local):   vlc udp://@:5004
#                        ffplay udp://@127.0.0.1:5004
#   Multicast (default): sudo ip route add 239.0.0.0/8 dev lo
#                        vlc udp://@239.1.1.1:5004

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UE_PROJECT="${REPO_ROOT}/unreal_project/CamSimTest/CamSimTest.uproject"
STAGED_DIR="${REPO_ROOT}/unreal_project/CamSimTest/Saved/StagedBuilds/Linux/CamSimTest"
LOG_DIR="${REPO_ROOT}/unreal_project/CamSimTest/Saved/Logs"

# -------------------------------------------------------------------------
# Defaults
# -------------------------------------------------------------------------
MODE=""
DO_BUILD=0
BUILD_ONLY=0
HEADLESS=0
FOLLOW_LOG=0
USE_LOCAL=0
declare -a EXTRA_ARGS=()

# -------------------------------------------------------------------------
# Parse arguments
# -------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build)       DO_BUILD=1;                            shift   ;;
        --build-only)  DO_BUILD=1; BUILD_ONLY=1;             shift   ;;
        --mode)        MODE="$2";                             shift 2 ;;
        --headless)    HEADLESS=1;                            shift   ;;
        --local)       USE_LOCAL=1;                           shift   ;;
        --cigi-port)   export CAMSIM_CIGI_PORT="$2";         shift 2 ;;
        --multicast)   export CAMSIM_MULTICAST_ADDR="$2";    shift 2 ;;
        --mcast-port)  export CAMSIM_MULTICAST_PORT="$2";    shift 2 ;;
        --log)         FOLLOW_LOG=1;                          shift   ;;
        --help|-h)
            sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)  EXTRA_ARGS+=("$1");                               shift   ;;
    esac
done

# -------------------------------------------------------------------------
# Locate UE installation (needed for both build and launch)
# -------------------------------------------------------------------------
UE_SEARCH_PATHS=(
    "${HOME}/UnrealEngine/Engine/Binaries/Linux/UnrealEditor"
    "/opt/UnrealEngine/Engine/Binaries/Linux/UnrealEditor"
    "/opt/Epic/UE_5.7/Engine/Binaries/Linux/UnrealEditor"
    "${HOME}/.local/share/UnrealEngine/Engine/Binaries/Linux/UnrealEditor"
)
UE_BINARY="${UE_BINARY:-}"
for CANDIDATE in "${UE_SEARCH_PATHS[@]}"; do
    if [ -f "${CANDIDATE}" ]; then
        UE_BINARY="${CANDIDATE}"; break
    fi
done
if [ -z "${UE_BINARY}" ]; then
    UE_BINARY="$(find /opt "${HOME}" -maxdepth 6 -name UnrealEditor \
        -path '*/Binaries/Linux/UnrealEditor' 2>/dev/null | head -1 || true)"
fi
if [ -z "${UE_BINARY}" ]; then
    echo "[ERROR] UnrealEditor not found."
    echo "        Install UE 5.7 from source or via Epic Games Launcher, or:"
    echo "        export UE_BINARY=/path/to/UnrealEditor"
    exit 1
fi

# Derive UE install root from the binary path:
#   .../UE_5.7/Engine/Binaries/Linux/UnrealEditor → .../UE_5.7
UE_ROOT="${UE_BINARY%%/Engine/Binaries/*}"
UBT="${UE_ROOT}/Engine/Build/BatchFiles/Linux/Build.sh"

# -------------------------------------------------------------------------
# Auto-detect run mode
# -------------------------------------------------------------------------
if [ -z "${MODE}" ]; then
    if [ -d "${STAGED_DIR}" ]; then MODE="packaged"; else MODE="editor"; fi
fi

# -------------------------------------------------------------------------
# Build step
# -------------------------------------------------------------------------
if [ "${DO_BUILD}" -eq 1 ]; then

    if [ "${MODE}" = "packaged" ]; then
        # Packaged mode: UBT compile alone is not enough — game content must
        # also be cooked and staged before the binary can run.  Use RunUAT.
        UAT="${UE_ROOT}/Engine/Build/BatchFiles/RunUAT.sh"
        if [ ! -f "${UAT}" ]; then
            echo "[ERROR] RunUAT.sh not found: ${UAT}"
            exit 1
        fi

        STAGE_DIR="${REPO_ROOT}/unreal_project/CamSimTest/Saved/StagedBuilds"
        echo "==> Packaging CamSimTest Shipping (compile + cook + stage + pak) …"
        echo "    Output: ${STAGE_DIR}/Linux"
        echo "    (first run takes several minutes)"
        "${UAT}" BuildCookRun \
            -project="${UE_PROJECT}" \
            -platform=Linux \
            -clientconfig=Shipping \
            -build -cook -stage -pak \
            -stagingdirectory="${STAGE_DIR}" \
            -unattended -utf8output
    else
        # Editor mode: compile only — content is used uncooked from the project dir
        if [ ! -f "${UBT}" ]; then
            echo "[ERROR] Build.sh not found: ${UBT}"
            exit 1
        fi
        echo "==> Building CamSimTestEditor (Development) …"
        "${UBT}" \
            CamSimTestEditor \
            Linux \
            Development \
            "${UE_PROJECT}" \
            -waitmutex
    fi

    echo "==> Build complete."
    [ "${BUILD_ONLY}" -eq 1 ] && exit 0
fi

# -------------------------------------------------------------------------
# Output address / multicast route check
# -------------------------------------------------------------------------
if [ "${USE_LOCAL}" -eq 1 ]; then
    export CAMSIM_MULTICAST_ADDR="${CAMSIM_MULTICAST_ADDR:-127.0.0.1}"
    echo "==> Local (unicast) mode → udp://${CAMSIM_MULTICAST_ADDR}:${CAMSIM_MULTICAST_PORT:-5004}"
    echo "    Receive:  vlc udp://@:${CAMSIM_MULTICAST_PORT:-5004}"
    echo "              ffplay udp://@127.0.0.1:${CAMSIM_MULTICAST_PORT:-5004}"
else
    MCAST_ADDR="${CAMSIM_MULTICAST_ADDR:-239.1.1.1}"
    MCAST_PORT="${CAMSIM_MULTICAST_PORT:-5004}"
    echo "==> Multicast mode → udp://${MCAST_ADDR}:${MCAST_PORT}"
    if ! ip route show 2>/dev/null | grep -qE "^2(2[4-9]|3[0-9])\."; then
        echo "    [WARN] No multicast route — packets will be dropped silently."
        echo "    Fix:  sudo ip route add 239.0.0.0/8 dev lo"
        echo "    Or:   relaunch with --local"
        echo ""
        read -r -t 10 -p "    Add route now? (sudo) [y/N]: " ADD_ROUTE || true
        if [[ "${ADD_ROUTE:-}" =~ ^[Yy]$ ]]; then
            sudo ip route add 239.0.0.0/8 dev lo
        fi
    fi
fi

# -------------------------------------------------------------------------
# Config file — copy deploy/camsim_config.json into the project directory
# so FCamSimConfig::Load() finds it via FPaths::ProjectDir()
# -------------------------------------------------------------------------
CONFIG_SRC="${REPO_ROOT}/deploy/camsim_config.json"
CONFIG_DST="${REPO_ROOT}/unreal_project/CamSimTest/camsim_config.json"
if [ -f "${CONFIG_SRC}" ]; then
    cp -f "${CONFIG_SRC}" "${CONFIG_DST}"
    echo "==> Config: ${CONFIG_DST}"
elif [ ! -f "${CONFIG_DST}" ]; then
    echo "[WARN] No camsim_config.json found — entity types will not be loaded."
    echo "       Expected: ${CONFIG_SRC}"
fi

# -------------------------------------------------------------------------
# Vulkan ICD selection
# -------------------------------------------------------------------------
if [ -z "${VK_ICD_FILENAMES:-}" ]; then
    if [ -f /usr/share/vulkan/icd.d/nvidia_icd.json ]; then
        export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json
        echo "==> Vulkan: NVIDIA ICD"
    elif [ -f /usr/share/vulkan/icd.d/lvp_icd.x86_64.json ]; then
        # CPU fallback (Mesa llvmpipe) — disable ray tracing
        export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
        EXTRA_ARGS+=("-ini:Engine:[/Script/Engine.RendererSettings]:r.RayTracing=False")
        echo "==> Vulkan: Mesa llvmpipe (CPU) — ray tracing disabled"
    else
        echo "==> Vulkan: auto-detect (set VK_ICD_FILENAMES to override)"
    fi
fi

# -------------------------------------------------------------------------
# Common UE arguments
# -------------------------------------------------------------------------
UE_COMMON_ARGS=( "/Game/Main?game=/Script/CamSimTest.CamSimGameMode" "-nosound" "-unattended" "-log" "-vulkan" )
[ "${HEADLESS}" -eq 1 ] && UE_COMMON_ARGS+=("-RenderOffScreen")

echo "==> Mode: ${MODE}$([ "${HEADLESS}" -eq 1 ] && echo " (headless)")"

# -------------------------------------------------------------------------
# Launch
# -------------------------------------------------------------------------
if [ "${MODE}" = "packaged" ]; then
    BINARY="${STAGED_DIR}/Binaries/Linux/CamSimTest-Linux-Shipping"
    [ ! -f "${BINARY}" ] && {
        echo "[ERROR] Packaged binary not found: ${BINARY}"
        echo "        Build it first: ./scripts/run_linux.sh --build-only --mode packaged"
        exit 1
    }
    LOG_FILE="${STAGED_DIR}/Saved/Logs/CamSimTest.log"
    echo "    Binary: ${BINARY}"
    "${BINARY}" "${UE_COMMON_ARGS[@]}" "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}" &
else
    LOG_FILE="${LOG_DIR}/CamSimTest.log"
    echo "    Editor: ${UE_BINARY}"
    "${UE_BINARY}" "${UE_PROJECT}" -game "${UE_COMMON_ARGS[@]}" "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}" &
fi
UE_PID=$!

echo "==> UE launched (PID ${UE_PID})"
echo "    Log: ${LOG_FILE}"
echo ""

# -------------------------------------------------------------------------
# Log tail
# -------------------------------------------------------------------------
if [ "${FOLLOW_LOG}" -eq 1 ]; then
    for _ in $(seq 1 40); do [ -f "${LOG_FILE}" ] && break; sleep 0.5; done
    if [ -f "${LOG_FILE}" ]; then
        echo "==> Tailing log (Ctrl-C stops tail; UE keeps running)"
        tail -F "${LOG_FILE}" \
            | grep --line-buffered -E '(LogCamSim|Error|Warning)' &
        TAIL_PID=$!
        trap 'kill "${TAIL_PID}" 2>/dev/null || true; echo' INT TERM
        wait "${UE_PID}" 2>/dev/null || true
        kill "${TAIL_PID}" 2>/dev/null || true
    else
        echo "[WARN] Log not found after 20s: ${LOG_FILE}"
    fi
else
    echo "    Tip: rerun with --log, or:"
    echo "    grep LogCamSim '${LOG_FILE}'"
fi
