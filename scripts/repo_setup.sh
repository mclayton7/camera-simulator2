#!/usr/bin/env bash
# repo_setup.sh
#
# One-time setup: clone glTFRuntime into the UE project's Plugins directory
# and install Cesium for Unreal into the engine's Plugins/Marketplace.
#
# Run this once after cloning the repository, before opening the project.
#
# Usage:
#   ./scripts/repo_setup.sh
#   UE_ROOT=/path/to/UE_5.8 ./scripts/repo_setup.sh   # pick the engine explicitly

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

GLTF_REPO="https://github.com/rdeioris/glTFRuntime.git"
# Keep CESIUM_VERSION / CESIUM_UE in step with the engine version in
# CamSimTest.uproject (EngineAssociation) and the *.Target.cs files.
CESIUM_VERSION="2.29.1"
CESIUM_UE="58"
CESIUM_REPO="https://github.com/CesiumGS/cesium-unreal/releases/download/v${CESIUM_VERSION}/CesiumForUnreal-${CESIUM_UE}-v${CESIUM_VERSION}.zip"

PLUGIN_DIR="${SCRIPT_DIR}/../unreal_project/CamSimTest/Plugins"
CESIUM_ZIP="${SCRIPT_DIR}/../.build_tmp/CesiumForUnreal.zip"

mkdir -p "${PLUGIN_DIR}" "$(dirname "${CESIUM_ZIP}")"

# Clone the glTFRuntime repository
if [ -d "${PLUGIN_DIR}/glTFRuntime" ]; then
    # Fast-forward existing clones so engine upgrades pick up upstream fixes.
    echo "==> glTFRuntime already present, updating..."
    git -C "${PLUGIN_DIR}/glTFRuntime" pull --ff-only --quiet \
        || echo "    [WARN] glTFRuntime update failed; continuing with existing checkout."
else
    echo "==> Cloning glTFRuntime..."
    git clone "${GLTF_REPO}" --depth 1 --branch master --single-branch \
        "${PLUGIN_DIR}/glTFRuntime"
fi

# Cesium for Unreal goes into the engine's Plugins/Marketplace directory, as
# the release zip intends. UBT then uses the zip's prebuilt binaries. A copy in
# the project's Plugins/ is rebuilt from source with the project's settings
# (~20 min), and 2.29.1 does not compile that way under Apple clang 21.
#
# Set UE_ROOT to pick the engine explicitly (the directory containing Engine/).
UE_ROOT="${UE_ROOT:-}"
if [ -z "${UE_ROOT}" ]; then
    for CANDIDATE in \
        "/Users/Shared/Epic Games/UE_5.8" \
        "/opt/UE" \
        "/opt/Epic/UE_5.8" \
        "/opt/UnrealEngine" \
        "${HOME}/UnrealEngine" \
        "${HOME}/.local/share/UnrealEngine"; do
        [ -f "${CANDIDATE}/Engine/Build/Build.version" ] && { UE_ROOT="${CANDIDATE}"; break; }
    done
fi
if [ -z "${UE_ROOT}" ] || [ ! -f "${UE_ROOT}/Engine/Build/Build.version" ]; then
    echo "[ERROR] UE 5.8 engine not found. Set UE_ROOT to the directory containing Engine/." >&2
    exit 1
fi
ENGINE_VERSION="$(sed -n 's/.*"MajorVersion": *\([0-9]*\).*/\1/p' "${UE_ROOT}/Engine/Build/Build.version")$(sed -n 's/.*"MinorVersion": *\([0-9]*\).*/\1/p' "${UE_ROOT}/Engine/Build/Build.version")"
if [ "${ENGINE_VERSION}" != "${CESIUM_UE}" ]; then
    echo "[ERROR] ${UE_ROOT} is UE ${ENGINE_VERSION}, but Cesium ${CESIUM_VERSION} is set up for UE ${CESIUM_UE}." >&2
    exit 1
fi

MARKETPLACE_DIR="${UE_ROOT}/Engine/Plugins/Marketplace"
CESIUM_DIR="${MARKETPLACE_DIR}/CesiumForUnreal"
BACKUP_DIR="$(dirname "${CESIUM_ZIP}")"

cesium_version() {
    [ -f "$1/CesiumForUnreal.uplugin" ] \
        && sed -n 's/.*"VersionName": *"\([^"]*\)".*/\1/p' "$1/CesiumForUnreal.uplugin"
    return 0
}

# A project-local copy would override the engine one, so move it aside.
if [ -d "${PLUGIN_DIR}/CesiumForUnreal" ]; then
    OLD_VERSION="$(cesium_version "${PLUGIN_DIR}/CesiumForUnreal")"
    OLD_CESIUM="${BACKUP_DIR}/CesiumForUnreal-project-v${OLD_VERSION:-unknown}.bak"
    echo "==> Moving project-local CesiumForUnreal ${OLD_VERSION:-unknown} to ${OLD_CESIUM}"
    rm -rf "${OLD_CESIUM}"
    mv "${PLUGIN_DIR}/CesiumForUnreal" "${OLD_CESIUM}"
fi

INSTALLED_CESIUM="$(cesium_version "${CESIUM_DIR}")"
if [ "${INSTALLED_CESIUM}" = "${CESIUM_VERSION}" ]; then
    echo "==> CesiumForUnreal ${CESIUM_VERSION} already installed in ${MARKETPLACE_DIR}, skipping download."
else
    if ! mkdir -p "${MARKETPLACE_DIR}" 2>/dev/null || [ ! -w "${MARKETPLACE_DIR}" ]; then
        echo "[ERROR] ${MARKETPLACE_DIR} is not writable." >&2
        echo "        Make it writable by this user, or re-run this script with sudo." >&2
        exit 1
    fi
    if [ -d "${CESIUM_DIR}" ]; then
        OLD_CESIUM="${BACKUP_DIR}/CesiumForUnreal-v${INSTALLED_CESIUM:-unknown}.bak"
        echo "==> Replacing CesiumForUnreal ${INSTALLED_CESIUM:-unknown} (moved to ${OLD_CESIUM})"
        rm -rf "${OLD_CESIUM}"
        mv "${CESIUM_DIR}" "${OLD_CESIUM}"
    fi
    echo "==> Downloading Cesium for Unreal ${CESIUM_VERSION} (UE ${CESIUM_UE})..."
    curl -fSL "${CESIUM_REPO}" -o "${CESIUM_ZIP}"
    echo "==> Extracting to ${MARKETPLACE_DIR}..."
    # Extract beside the target and move into place, so an interrupted unzip
    # never leaves a half-installed plugin that the version check accepts.
    EXTRACT_DIR="$(mktemp -d "${MARKETPLACE_DIR}/.cesium-extract.XXXXXX")"
    unzip -q "${CESIUM_ZIP}" -d "${EXTRACT_DIR}"
    mv "${EXTRACT_DIR}/CesiumForUnreal" "${CESIUM_DIR}"
    rm -rf "${EXTRACT_DIR}" "${CESIUM_ZIP}"
fi

echo ""
echo "==> Plugin setup complete."
echo "    You can now open the project in UE5 or run ./scripts/build_thirdparty.sh"
