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