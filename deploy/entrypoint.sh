#!/usr/bin/env bash
# CamSim Docker entrypoint
# Detects GPU availability and selects appropriate Vulkan ICD, then launches UE.
set -euo pipefail

BINARY_DIR=/opt/camsim
GAME_BINARY="${BINARY_DIR}/CamSimTest/Binaries/Linux/CamSimTest-Linux-Shipping"
CONFIG_SRC="${BINARY_DIR}/camsim_config.json"
CONFIG_DST="${BINARY_DIR}/CamSimTest/Binaries/Linux/camsim_config.json"

# -----------------------------------------------------------------------
# Copy default config if not already present (allows volume-mount override)
# -----------------------------------------------------------------------
if [ -f "${CONFIG_SRC}" ] && [ ! -f "${CONFIG_DST}" ]; then
    cp "${CONFIG_SRC}" "${CONFIG_DST}"
fi

# -----------------------------------------------------------------------
# Vulkan ICD selection
# -----------------------------------------------------------------------
EXTRA_ARGS=""

if [ -n "${NVIDIA_VISIBLE_DEVICES:-}" ] && [ "${NVIDIA_VISIBLE_DEVICES}" != "void" ]; then
    echo "[entrypoint] NVIDIA GPU detected (NVIDIA_VISIBLE_DEVICES=${NVIDIA_VISIBLE_DEVICES})"
    # NVIDIA Vulkan ICD installed by nvidia-container-toolkit
    export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json
else
    echo "[entrypoint] No NVIDIA GPU — using Mesa llvmpipe (CPU Vulkan)"
    export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
    # Disable ray tracing on CPU path (llvmpipe doesn't support it)
    EXTRA_ARGS="-ini:Engine:[/Script/Engine.RendererSettings]:r.RayTracing=False"
fi

# -----------------------------------------------------------------------
# Virtual display — SDL2 requires an X11 display even in -RenderOffScreen
# mode.  Start Xvfb so SDL can initialise; UE renders via Vulkan offscreen
# and never actually draws to the framebuffer.
# -----------------------------------------------------------------------
Xvfb :1 -screen 0 1280x720x24 -nolisten tcp &
XVFB_PID=$!
export DISPLAY=:1
echo "[entrypoint] Xvfb started (PID ${XVFB_PID}, DISPLAY=${DISPLAY})"

# -----------------------------------------------------------------------
# SIGTERM → clean shutdown
# -----------------------------------------------------------------------
_term() {
    echo "[entrypoint] SIGTERM received — forwarding to UE process"
    kill -TERM "${UE_PID}" 2>/dev/null || true
}
trap _term SIGTERM SIGINT

# -----------------------------------------------------------------------
# Launch Unreal Engine
# -----------------------------------------------------------------------
echo "[entrypoint] Launching: ${GAME_BINARY}"
"${GAME_BINARY}" \
    /Game/Main \
    -RenderOffScreen \
    -nosound \
    -unattended \
    -vulkan \
    -log \
    ${EXTRA_ARGS} \
    "$@" &

UE_PID=$!
echo "[entrypoint] UE PID = ${UE_PID}"
wait "${UE_PID}"
EXIT_CODE=$?
echo "[entrypoint] UE exited with code ${EXIT_CODE}"
kill "${XVFB_PID}" 2>/dev/null || true
exit "${EXIT_CODE}"
