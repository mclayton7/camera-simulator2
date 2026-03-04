#!/usr/bin/env bash
# repo_setup.sh
#
# One-time setup: clone/download third-party plugins (glTFRuntime and
# Cesium for Unreal) into the UE project's Plugins directory.
#
# Run this once after cloning the repository, before opening the project.
#
# Usage:
#   ./scripts/repo_setup.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

GLTF_REPO="https://github.com/rdeioris/glTFRuntime.git"
CESIUM_REPO="https://github.com/CesiumGS/cesium-unreal/releases/download/v2.23.0/CesiumForUnreal-57-v2.23.0.zip"

PLUGIN_DIR="${SCRIPT_DIR}/../unreal_project/CamSimTest/Plugins"
CESIUM_ZIP="${SCRIPT_DIR}/../.build_tmp/CesiumForUnreal.zip"

mkdir -p "${PLUGIN_DIR}" "$(dirname "${CESIUM_ZIP}")"

# Clone the glTFRuntime repository
if [ -d "${PLUGIN_DIR}/glTFRuntime" ]; then
    echo "==> glTFRuntime already present, skipping clone."
else
    echo "==> Cloning glTFRuntime..."
    git clone "${GLTF_REPO}" --depth 1 --branch master --single-branch \
        "${PLUGIN_DIR}/glTFRuntime"
fi

# Download and extract the Cesium for Unreal plugin
if [ -d "${PLUGIN_DIR}/CesiumForUnreal" ]; then
    echo "==> CesiumForUnreal already present, skipping download."
else
    echo "==> Downloading Cesium for Unreal..."
    curl -fSL "${CESIUM_REPO}" -o "${CESIUM_ZIP}"
    echo "==> Extracting..."
    unzip -q "${CESIUM_ZIP}" -d "${PLUGIN_DIR}"
    rm "${CESIUM_ZIP}"
fi

# Patch: CesiumCartographicPolygon.cpp uses ACesiumGeoreference methods but
# never includes CesiumGeoreference.h — only gets a forward declaration via
# CesiumGlobeAnchorComponent.h, causing an incomplete-type error on Linux.
POLYGON_CPP="${PLUGIN_DIR}/CesiumForUnreal/Source/CesiumRuntime/Private/CesiumCartographicPolygon.cpp"
if [ -f "${POLYGON_CPP}" ] && ! grep -q '"CesiumGeoreference.h"' "${POLYGON_CPP}"; then
    # Use a portable sed command (no -i '' vs -i difference needed here because
    # we write via a temp file)
    TMP_CPP="$(mktemp)"
    sed 's|#include "CesiumActors.h"|#include "CesiumActors.h"\n#include "CesiumGeoreference.h"|' \
        "${POLYGON_CPP}" > "${TMP_CPP}"
    mv "${TMP_CPP}" "${POLYGON_CPP}"
    echo "==> Patched CesiumCartographicPolygon.cpp: added #include \"CesiumGeoreference.h\""
fi

echo ""
echo "==> Plugin setup complete."
echo "    You can now open the project in UE5 or run ./scripts/build_thirdparty.sh"
