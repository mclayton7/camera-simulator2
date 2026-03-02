#!/bin/bash
SCRIPT_DIR=$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")

GLTF_REPO=https://github.com/rdeioris/glTFRuntime.git

CESIUM_REPO=https://github.com/CesiumGS/cesium-unreal/releases/download/v2.23.0/CesiumForUnreal-57-v2.23.0.zip

PLUGIN_DIR=$SCRIPT_DIR/../unreal_project/CamSimTest/Plugins

mkdir -p $PLUGIN_DIR
# Clone the glTFRuntime repository
git clone $GLTF_REPO --depth 1 --branch master --single-branch $PLUGIN_DIR/glTFRuntime

# Download and extract the Cesium for Unreal plugin
curl -L $CESIUM_REPO -o CesiumForUnreal.zip
unzip CesiumForUnreal.zip -d $PLUGIN_DIR
rm CesiumForUnreal.zip

# Patch: CesiumCartographicPolygon.cpp uses ACesiumGeoreference methods but
# never includes CesiumGeoreference.h — only gets a forward declaration via
# CesiumGlobeAnchorComponent.h, which causes an incomplete-type error on Linux.
POLYGON_CPP="$PLUGIN_DIR/CesiumForUnreal/Source/CesiumRuntime/Private/CesiumCartographicPolygon.cpp"
if ! grep -q '"CesiumGeoreference.h"' "$POLYGON_CPP"; then
    sed -i 's|#include "CesiumActors.h"|#include "CesiumActors.h"\n#include "CesiumGeoreference.h"|' "$POLYGON_CPP"
    echo "Patched CesiumCartographicPolygon.cpp: added #include \"CesiumGeoreference.h\""
fi