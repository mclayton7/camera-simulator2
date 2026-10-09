// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FCamSimConfig;

/**
 * Scene packages (REALISM R0, docs/scene-packages.md) and the local file URLs Cesium reads them through.
 */
namespace CamSimScene
{
	/** manifest.json schema_version this build reads. */
	constexpr int32 SupportedSchemaVersion = 1;

	/**
	 * Absolute path -> file:/// URL. Backslashes become '/', and every byte outside
	 * [A-Za-z0-9-._~/:] is percent-encoded (UTF-8), as Cesium's UnrealAssetAccessor decodes it.
	 */
	FString PathToFileUrl(const FString& AbsolutePath);

	/**
	 * file:/// URL -> native path (percent-decoded; "/C:/x" -> "C:/x"). False for anything
	 * that doesn't start with "file:///" (Cesium sends file://host/ and http(s) to the network).
	 */
	bool FileUrlToPath(const FString& Url, FString& OutPath);

	/**
	 * Apply scene.dir to Cfg: each layer the package has (terrain/layer.json,
	 * imagery/tilemapresource.xml, landcover/index.json) replaces cesium.terrain, cesium.imagery
	 * and thermal.land_cover.dir. Fills Cfg.Scene.PackageName / ResolveErrors / ResolveWarnings
	 * (Validate() and ValidateWarnings() report them). No-op when scene.dir is empty.
	 */
	void ResolvePackage(FCamSimConfig& Cfg);
}
