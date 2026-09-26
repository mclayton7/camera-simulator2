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

# Download and extract the Cesium for Unreal plugin. An existing install at a
# different version (e.g. left over from an engine upgrade) is moved aside to
# .build_tmp/ and replaced.
CESIUM_UPLUGIN="${PLUGIN_DIR}/CesiumForUnreal/CesiumForUnreal.uplugin"
INSTALLED_CESIUM=""
if [ -f "${CESIUM_UPLUGIN}" ]; then
    INSTALLED_CESIUM="$(sed -n 's/.*"VersionName": *"\([^"]*\)".*/\1/p' "${CESIUM_UPLUGIN}")"
fi
if [ "${INSTALLED_CESIUM}" = "${CESIUM_VERSION}" ]; then
    echo "==> CesiumForUnreal ${CESIUM_VERSION} already present, skipping download."
else
    if [ -d "${PLUGIN_DIR}/CesiumForUnreal" ]; then
        OLD_CESIUM="$(dirname "${CESIUM_ZIP}")/CesiumForUnreal-v${INSTALLED_CESIUM:-unknown}.bak"
        echo "==> Replacing CesiumForUnreal ${INSTALLED_CESIUM:-unknown} (moved to ${OLD_CESIUM})"
        rm -rf "${OLD_CESIUM}"
        mv "${PLUGIN_DIR}/CesiumForUnreal" "${OLD_CESIUM}"
    fi
    echo "==> Downloading Cesium for Unreal ${CESIUM_VERSION} (UE ${CESIUM_UE})..."
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

# Patch: IonQuickAddPanel.cpp (Cesium 2.29.1) captures AssetDepotConfirmWindow
# by reference inside its own initializer, which Apple clang 21 rejects
# (-Werror,-Wuninitialized). Declare the pointer first, then assign it; the
# modal window blocks until closed, so the by-reference capture stays valid.
QUICKADD_CPP="${PLUGIN_DIR}/CesiumForUnreal/Source/CesiumEditor/Private/IonQuickAddPanel.cpp"
if [ -f "${QUICKADD_CPP}" ] && grep -q 'TSharedRef<SWindow> AssetDepotConfirmWindow =' "${QUICKADD_CPP}"; then
    perl -0pi -e '
        s/TSharedRef<SWindow> AssetDepotConfirmWindow =(\r?\n)/TSharedPtr<SWindow> AssetDepotConfirmWindow;$1  AssetDepotConfirmWindow =$1/;
        s/EditorAddModalWindow\(AssetDepotConfirmWindow\)/EditorAddModalWindow(AssetDepotConfirmWindow.ToSharedRef())/;
    ' "${QUICKADD_CPP}"
    echo "==> Patched IonQuickAddPanel.cpp: AssetDepotConfirmWindow self-capture"
fi

# Patch: UE 5.8 editor targets on an installed engine must use
# BuildSettingsVersion.V7, which makes unreachable code an error for every
# module, plugins included. Cesium 2.29.1 has unreachable code (e.g.
# CesiumGaussianSplatSubsystem.cpp), so downgrade it to a warning in Cesium's
# own module rules.
for CESIUM_BUILD_CS in "${PLUGIN_DIR}"/CesiumForUnreal/Source/Cesium{Runtime,Editor}/*.Build.cs; do
    if [ -f "${CESIUM_BUILD_CS}" ] && ! grep -q 'UnreachableCodeWarningLevel' "${CESIUM_BUILD_CS}"; then
        perl -0pi -e 's/(: base\(Target\)\r?\n\s*\{(\r?\n))/$1        CppCompileWarningSettings.UnreachableCodeWarningLevel = WarningLevel.Warning;$2/' \
            "${CESIUM_BUILD_CS}"
        echo "==> Patched $(basename "${CESIUM_BUILD_CS}"): unreachable code is a warning"
    fi
done

echo ""
echo "==> Plugin setup complete."
echo "    You can now open the project in UE5 or run ./scripts/build_thirdparty.sh"
