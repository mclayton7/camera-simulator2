#!/usr/bin/env bash
# Regenerate Content/Ocean/M_Ocean + MPC_Ocean from scripts/ocean/make_ocean_material.py.
# Runs the pythonscript commandlet with a real RHI (-AllowCommandletRendering) so M_Ocean's shaders
# compile; a cold shader cache makes the first run take ~15 min.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
UE_CMD="${UE_CMD:-/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor}"
mkdir -p "$REPO/.cache"
# Regenerate from scratch: the script creates both assets and refuses to overwrite in-process.
rm -f "$REPO/unreal_project/CamSimTest/Content/Ocean/M_Ocean.uasset" "$REPO/unreal_project/CamSimTest/Content/Ocean/MPC_Ocean.uasset"
"$UE_CMD" "$REPO/unreal_project/CamSimTest/CamSimTest.uproject" -run=pythonscript \
  -script="$REPO/scripts/ocean/make_ocean_material.py" -AllowCommandletRendering -unattended -nosplash -nosound -stdout -FullStdOutLogOutput \
  > "$REPO/.cache/make_ocean_material.log" 2>&1 || true
RESTORE="The old assets were deleted; restore them with: git checkout -- unreal_project/CamSimTest/Content/Ocean"
# Two forms: "LogShaderCompilers: ... Failed to compile Material /Game/Ocean/M_Ocean.M_Ocean ..." (shader errors)
# and "LogMaterial: ... /Game/Ocean/M_Ocean.M_Ocean: Failed to compile Material ..." (graph errors).
if grep -E -A3 "Failed to compile Material /Game/Ocean/M_Ocean|/Game/Ocean/M_Ocean\.M_Ocean: Failed to compile" "$REPO/.cache/make_ocean_material.log" >&2; then
  echo "M_Ocean failed to compile — see .cache/make_ocean_material.log. $RESTORE" >&2; exit 1
fi
grep -q OCEAN_MATERIAL_OK "$REPO/.cache/make_ocean_material.log" || { echo "failed — see .cache/make_ocean_material.log. $RESTORE" >&2; exit 1; }
echo "M_Ocean + MPC_Ocean written"
