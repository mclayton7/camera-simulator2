#!/usr/bin/env bash
# run.sh
#
# Build (optional) and launch CamSim for development and testing.
# Supports macOS and Linux. Detects the platform automatically.
#
# Usage:
#   ./scripts/run.sh [options]
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
#   ./scripts/run.sh --build --local --log
#
# Receiving the stream:
#   Unicast (--local):   vlc udp://@:5004
#                        ffplay udp://@127.0.0.1:5004
#   Multicast — macOS:   sudo route add -net 239.0.0.0/8 -interface lo0
#                        vlc udp://@239.1.1.1:5004
#   Multicast — Linux:   sudo ip route add 239.0.0.0/8 dev lo
#                        vlc udp://@239.1.1.1:5004

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UE_PROJECT="${REPO_ROOT}/unreal_project/CamSimTest/CamSimTest.uproject"
LOG_DIR="${REPO_ROOT}/unreal_project/CamSimTest/Saved/Logs"

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
# Locate UE installation
# -------------------------------------------------------------------------
UE_BINARY="${UE_BINARY:-}"

if [ "${PLATFORM}" = "mac" ]; then
    STAGED_APP="${REPO_ROOT}/unreal_project/CamSimTest/Saved/StagedBuilds/Mac/CamSimTest.app"
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
    STAGED_DIR="${REPO_ROOT}/unreal_project/CamSimTest/Saved/StagedBuilds/Linux/CamSimTest"
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
    echo "[ERROR] UnrealEditor not found."
    echo "        Install UE 5.7 via Epic Games Launcher (macOS) or from source (Linux), or:"
    echo "        export UE_BINARY=/path/to/UnrealEditor"
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

# -------------------------------------------------------------------------
# Auto-detect run mode
# -------------------------------------------------------------------------
if [ -z "${MODE}" ]; then
    if [ "${PLATFORM}" = "mac" ] && [ -d "${STAGED_APP}" ]; then
        MODE="packaged"
    elif [ "${PLATFORM}" = "linux" ] && [ -d "${STAGED_DIR:-}" ]; then
        MODE="packaged"
    else
        MODE="editor"
    fi
fi

# -------------------------------------------------------------------------
# Build step
# -------------------------------------------------------------------------
if [ "${DO_BUILD}" -eq 1 ]; then
    if [ "${MODE}" = "packaged" ]; then
        UAT="${UE_ROOT}/Engine/Build/BatchFiles/RunUAT.sh"
        if [ ! -f "${UAT}" ]; then
            echo "[ERROR] RunUAT.sh not found: ${UAT}"
            exit 1
        fi
        STAGE_DIR="${REPO_ROOT}/unreal_project/CamSimTest/Saved/StagedBuilds"
        echo "==> Packaging CamSimTest Shipping (compile + cook + stage + pak) …"
        echo "    Output: ${STAGE_DIR}/${UBT_PLATFORM}"
        echo "    (first run takes several minutes)"
        "${UAT}" BuildCookRun \
            -project="${UE_PROJECT}" \
            -platform="${UBT_PLATFORM}" \
            -clientconfig=Shipping \
            -build -cook -stage -pak \
            -stagingdirectory="${STAGE_DIR}" \
            -unattended -utf8output
    else
        if [ ! -f "${UBT}" ]; then
            echo "[ERROR] Build.sh not found: ${UBT}"
            exit 1
        fi
        echo "==> Building CamSimTestEditor (Development) …"
        "${UBT}" \
            CamSimTestEditor \
            "${UBT_PLATFORM}" \
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
    if [ "${PLATFORM}" = "mac" ]; then
        MCAST_HAS_ROUTE="$(netstat -rn 2>/dev/null | grep -cE '^2(2[4-9]|3[0-9])\.' || true)"
        MCAST_ADD_CMD="sudo route add -net 239.0.0.0/8 -interface lo0"
    else
        MCAST_HAS_ROUTE="$(ip route show 2>/dev/null | grep -cE '^2(2[4-9]|3[0-9])\.' || true)"
        MCAST_ADD_CMD="sudo ip route add 239.0.0.0/8 dev lo"
    fi
    if [ "${MCAST_HAS_ROUTE}" = "0" ]; then
        echo "    [WARN] No multicast route — packets will be dropped silently."
        echo "    Fix:  ${MCAST_ADD_CMD}"
        echo "    Or:   relaunch with --local"
        echo ""
        read -r -t 10 -p "    Add route now? (sudo) [y/N]: " ADD_ROUTE || true
        if [[ "${ADD_ROUTE:-}" =~ ^[Yy]$ ]]; then
            eval "${MCAST_ADD_CMD}"
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
# Linux: Xvfb virtual display and Vulkan ICD selection
# -------------------------------------------------------------------------
XVFB_PID=""
if [ "${PLATFORM}" = "linux" ]; then
    if [ "${HEADLESS}" -eq 1 ] && [ -z "${DISPLAY:-}" ]; then
        if command -v Xvfb >/dev/null 2>&1; then
            XVFB_DISPLAY=":99"
            Xvfb "${XVFB_DISPLAY}" -screen 0 1280x720x24 -nolisten tcp &
            XVFB_PID=$!
            export DISPLAY="${XVFB_DISPLAY}"
            echo "==> Xvfb started on ${DISPLAY} (PID ${XVFB_PID})"
        else
            echo "[WARN] --headless requested but Xvfb not found and DISPLAY is unset."
            echo "       SDL will fail to initialise. Install: sudo apt-get install -y xvfb"
        fi
    fi

    if [ -z "${VK_ICD_FILENAMES:-}" ]; then
        if [ -f /usr/share/vulkan/icd.d/nvidia_icd.json ]; then
            export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json
            echo "==> Vulkan: NVIDIA ICD"
        elif [ -f /usr/share/vulkan/icd.d/lvp_icd.x86_64.json ]; then
            export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
            EXTRA_ARGS+=("-ini:Engine:[/Script/Engine.RendererSettings]:r.RayTracing=False")
            echo "==> Vulkan: Mesa llvmpipe (CPU) — ray tracing disabled"
        else
            echo "==> Vulkan: auto-detect (set VK_ICD_FILENAMES to override)"
        fi
    fi
fi

# -------------------------------------------------------------------------
# macOS headless: stamp LSUIElement so the packaged app has no Dock icon
# -------------------------------------------------------------------------
if [ "${PLATFORM}" = "mac" ] && [ "${HEADLESS}" -eq 1 ] && [ "${MODE}" = "packaged" ]; then
    PLIST="${STAGED_APP}/Contents/Info.plist"
    if [ -f "${PLIST}" ]; then
        if ! /usr/libexec/PlistBuddy -c "Print :LSUIElement" "${PLIST}" &>/dev/null; then
            /usr/libexec/PlistBuddy -c "Add :LSUIElement bool true" "${PLIST}"
            echo "==> Info.plist: added LSUIElement=true (no Dock icon)"
        fi
    fi
fi

# -------------------------------------------------------------------------
# Common UE arguments
# -------------------------------------------------------------------------
UE_COMMON_ARGS=( "/Game/Main?game=/Script/CamSimTest.CamSimGameMode" "-nosound" "-unattended" "-log" )
[ "${PLATFORM}" = "linux" ] && UE_COMMON_ARGS+=("-vulkan")
[ "${HEADLESS}" -eq 1 ]     && UE_COMMON_ARGS+=("-RenderOffScreen")

echo "==> Platform: ${PLATFORM}  Mode: ${MODE}$([ "${HEADLESS}" -eq 1 ] && echo " (headless)")"

# -------------------------------------------------------------------------
# Launch
# -------------------------------------------------------------------------
if [ "${MODE}" = "packaged" ]; then
    if [ "${PLATFORM}" = "mac" ]; then
        BINARY="${STAGED_APP}/Contents/MacOS/CamSimTest"
        LOG_FILE="${HOME}/Library/Logs/CamSimTest/CamSimTest.log"
        RUN_MSG="--build-only --mode packaged"
    else
        BINARY="${STAGED_DIR}/Binaries/Linux/CamSimTest-Linux-Shipping"
        LOG_FILE="${STAGED_DIR}/Saved/Logs/CamSimTest.log"
        RUN_MSG="--build-only --mode packaged"
    fi
    [ ! -f "${BINARY}" ] && {
        echo "[ERROR] Packaged binary not found: ${BINARY}"
        echo "        Build it first: ./scripts/run.sh --build ${RUN_MSG}"
        exit 1
    }
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
        _cleanup() {
            kill "${TAIL_PID}" 2>/dev/null || true
            [ -n "${XVFB_PID}" ] && kill "${XVFB_PID}" 2>/dev/null || true
            echo
        }
        trap '_cleanup' INT TERM
        wait "${UE_PID}" 2>/dev/null || true
        _cleanup
    else
        echo "[WARN] Log not found after 20s: ${LOG_FILE}"
    fi
else
    echo "    Tip: rerun with --log, or:"
    echo "    grep LogCamSim '${LOG_FILE}'"
fi
