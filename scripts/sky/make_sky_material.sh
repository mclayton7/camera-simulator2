#!/usr/bin/env bash
# Regenerate Content/Sky/M_SkyDome + MPC_Sky from scripts/sky/make_sky_material.py.
# Runs the pythonscript commandlet with a real RHI (-AllowCommandletRendering) so M_SkyDome's shaders
# compile; a cold shader cache makes the first run take ~15 min.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
RHI_ARGS=()
XVFB_PID=""
if [[ "$(uname)" == "Linux" ]]; then
  UE_CMD="${UE_CMD:-/opt/UE/Engine/Binaries/Linux/UnrealEditor}"
  RHI_ARGS=(-vulkan)
  # SDL needs an X display even for a commandlet with rendering (as scripts/run_gpu_tests.sh).
  if [[ -z "${DISPLAY:-}" ]] && command -v Xvfb >/dev/null; then
    Xvfb :97 -screen 0 1280x720x24 -nolisten tcp >/dev/null 2>&1 & XVFB_PID=$!
    export DISPLAY=:97
    trap '[[ -n "$XVFB_PID" ]] && kill "$XVFB_PID" 2>/dev/null' EXIT
  fi
else
  UE_CMD="${UE_CMD:-/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor}"
fi
mkdir -p "$REPO/.cache"
# Regenerate from scratch: the script creates both assets and refuses to overwrite in-process.
rm -f "$REPO/unreal_project/CamSimTest/Content/Sky/M_SkyDome.uasset" "$REPO/unreal_project/CamSimTest/Content/Sky/MPC_Sky.uasset"
"$UE_CMD" "$REPO/unreal_project/CamSimTest/CamSimTest.uproject" -run=pythonscript \
  -script="$REPO/scripts/sky/make_sky_material.py" -AllowCommandletRendering "${RHI_ARGS[@]}" -unattended -nosplash -nosound -stdout -FullStdOutLogOutput \
  > "$REPO/.cache/make_sky_material.log" 2>&1 || true
RESTORE="The old assets were deleted; restore them with: git checkout -- unreal_project/CamSimTest/Content/Sky"
# Two forms: "LogShaderCompilers: ... Failed to compile Material /Game/Sky/M_SkyDome.M_SkyDome ..." (shader errors)
# and "LogMaterial: ... /Game/Sky/M_SkyDome.M_SkyDome: Failed to compile Material ..." (graph errors).
if grep -E -A3 "Failed to compile Material /Game/Sky/M_SkyDome|/Game/Sky/M_SkyDome\.M_SkyDome: Failed to compile" "$REPO/.cache/make_sky_material.log" >&2; then
  echo "M_SkyDome failed to compile — see .cache/make_sky_material.log. $RESTORE" >&2; exit 1
fi
grep -q SKY_MATERIAL_OK "$REPO/.cache/make_sky_material.log" || { echo "failed — see .cache/make_sky_material.log. $RESTORE" >&2; exit 1; }
echo "M_SkyDome + MPC_Sky written"
