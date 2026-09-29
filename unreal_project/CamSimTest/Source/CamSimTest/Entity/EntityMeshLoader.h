// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UStaticMesh;
class USkeletalMesh;

/**
 * CamSimMeshLoader
 *
 * Mesh loading helpers shared by ACamSimEntity (per-spawn loads) and
 * FEntityTypeTable (startup preload). Handles two asset kinds:
 *   - glTF (.gltf / .glb): loaded synchronously via the glTFRuntime plugin.
 *   - UE content (/Game/...): loaded via FSoftObjectPath::TryLoad().
 */
namespace CamSimMeshLoader
{
	/** Returns true for .gltf / .glb file paths (relative or absolute). */
	bool IsGltfPath(const FString& Path);

	/** Load a static mesh from a glTF file or a UE content path. */
	UStaticMesh* LoadStaticMesh(const FString& Path);

	/** Load a skeletal mesh from a glTF file or a UE content path. */
	USkeletalMesh* LoadSkeletalMesh(const FString& Path);
}
