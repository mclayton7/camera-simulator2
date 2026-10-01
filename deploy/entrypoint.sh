#!/usr/bin/env bash
# CamSim Docker entrypoint: pick the Vulkan driver, start a virtual display,
# launch the packaged game headless. Extra arguments are passed to UE.
set -euo pipefail

GAME_DIR=/opt/camsim
BIN_DIR="${GAME_DIR}/CamSimTest/Binaries/Linux"

# Development packages are named CamSimTest, Shipping ones CamSimTest-Linux-Shipping.
GAME_BINARY="${CAMSIM_BINARY:-}"
if [ -z "${GAME_BINARY}" ]; then
    for CANDIDATE in "${BIN_DIR}/CamSimTest" "${BIN_DIR}/CamSimTest-Linux-Shipping"; do
        [ -x "${CANDIDATE}" ] && { GAME_BINARY="${CANDIDATE}"; break; }
    done
fi
if [ -z "${GAME_BINARY}" ] || [ ! -x "${GAME_BINARY}" ]; then
    echo "[entrypoint] No game binary in ${BIN_DIR} (set CAMSIM_BINARY)" >&2
    exit 1
fi

# -----------------------------------------------------------------------
# Vulkan driver. Detect the GPU by its device node, not NVIDIA_VISIBLE_DEVICES:
# with CDI injection (toolkit >= 1.17, `--gpus all`) that variable reads
# "void" inside the container even though the GPU is present. The toolkit
# drops its ICD manifest in /etc/vulkan/icd.d, which the loader finds itself.
# -----------------------------------------------------------------------
EXTRA_ARGS=()
if compgen -G "/dev/nvidia[0-9]*" >/dev/null; then
    DEVICE="$(vulkaninfo --summary 2>/dev/null | sed -n 's/^\s*deviceName\s*=\s*//p' | head -1 || true)"
    if [ -z "${DEVICE}" ]; then
        echo "[entrypoint] NVIDIA device node present but Vulkan found no device." >&2
        echo "             Is NVIDIA_DRIVER_CAPABILITIES missing 'graphics'? vulkaninfo:" >&2
        vulkaninfo --summary 2>&1 | tail -5 >&2 || true
        exit 1
    fi
    echo "[entrypoint] Vulkan device: ${DEVICE}"
else
    echo "[entrypoint] No NVIDIA GPU passed in (run with --gpus all)." >&2
    if [ "${CAMSIM_ALLOW_SOFTWARE_RENDERING:-0}" != "1" ]; then
        echo "[entrypoint] CamSim needs a GPU: the sensor graph has no CPU fallback, and Mesa" >&2
        echo "             lavapipe crashes compiling UE 5.8 SM6 pipelines (Mesa 25.2/26.2," >&2
        echo "             docs/docker.md). CAMSIM_ALLOW_SOFTWARE_RENDERING=1 tries it anyway." >&2
        exit 1
    fi
    LVP_ICD="$(compgen -G "/usr/share/vulkan/icd.d/lvp_icd*.json" | head -1 || true)"
    echo "[entrypoint] CAMSIM_ALLOW_SOFTWARE_RENDERING=1: trying Mesa lavapipe (${LVP_ICD:-not found})"
    [ -n "${LVP_ICD}" ] && export VK_ICD_FILENAMES="${LVP_ICD}"
    # UE's device selection skips CPU devices without -AllowSoftwareRendering.
    # Lavapipe passes VP_UE_Vulkan_SM6 but not the RT profile (8 descriptor sets),
    # and caps allocations at 128 MB, below Nanite's default 512 MB streaming pool
    # (fatal at startup). CamSim draws no Nanite meshes.
    EXTRA_ARGS+=("-AllowSoftwareRendering"
                 "-ini:Engine:[ConsoleVariables]:r.Nanite.Streaming.StreamingPoolSize=64"
                 "-ini:Engine:[/Script/Engine.RendererSettings]:r.RayTracing=False")
fi

# -----------------------------------------------------------------------
# Virtual display: SDL needs an X display even with -RenderOffScreen; UE
# renders offscreen through Vulkan and never draws to it.
# -----------------------------------------------------------------------
Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/dev/null 2>&1 &
XVFB_PID=$!
export DISPLAY=:99

UE_PID=""
_term() {
    echo "[entrypoint] Stopping CamSim"
    [ -n "${UE_PID}" ] && kill -TERM "${UE_PID}" 2>/dev/null || true
}
trap _term TERM INT

echo "[entrypoint] Launching ${GAME_BINARY}"
"${GAME_BINARY}" \
    "/Game/Main?game=/Script/CamSimTest.CamSimGameMode" \
    -RenderOffScreen \
    -vulkan \
    -nosound \
    -unattended \
    -log \
    -userdir=/var/lib/camsim \
    "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}" \
    "$@" &
UE_PID=$!

# A trapped signal interrupts `wait`; wait again for UE's actual exit code.
EXIT_CODE=0
wait "${UE_PID}" || EXIT_CODE=$?
if kill -0 "${UE_PID}" 2>/dev/null; then
    EXIT_CODE=0
    wait "${UE_PID}" || EXIT_CODE=$?
fi
echo "[entrypoint] CamSim exited with code ${EXIT_CODE}"
kill "${XVFB_PID}" 2>/dev/null || true
exit "${EXIT_CODE}"
