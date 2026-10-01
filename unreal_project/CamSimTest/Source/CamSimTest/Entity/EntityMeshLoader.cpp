// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/EntityMeshLoader.h"
#include "Entity/EntityPaths.h"

#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/SoftObjectPath.h"
#include "glTFRuntimeFunctionLibrary.h"
#include "glTFRuntimeAsset.h"

namespace CamSimMeshLoader
{
	bool IsGltfPath(const FString& Path)
	{
		return Path.EndsWith(TEXT(".gltf"), ESearchCase::IgnoreCase) ||
		       Path.EndsWith(TEXT(".glb"),  ESearchCase::IgnoreCase);
	}

	UStaticMesh* LoadStaticMesh(const FString& Path)
	{
		if (IsGltfPath(Path))
		{
			FString AbsPath = CamSimEntityPaths::Resolve(Path);
			UglTFRuntimeAsset* Asset = UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilename(
				AbsPath, false, FglTFRuntimeConfig());
			if (!Asset) return nullptr;
			return Asset->LoadStaticMeshRecursive(TEXT(""), {}, FglTFRuntimeStaticMeshConfig());
		}
		return Cast<UStaticMesh>(FSoftObjectPath(Path).TryLoad());
	}

	USkeletalMesh* LoadSkeletalMesh(const FString& Path)
	{
		if (IsGltfPath(Path))
		{
			FString AbsPath = CamSimEntityPaths::Resolve(Path);
			UglTFRuntimeAsset* Asset = UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilename(
				AbsPath, false, FglTFRuntimeConfig());
			if (!Asset) return nullptr;
			return Asset->LoadSkeletalMeshRecursive(TEXT(""), {}, FglTFRuntimeSkeletalMeshConfig());
		}
		return Cast<USkeletalMesh>(FSoftObjectPath(Path).TryLoad());
	}
} // namespace CamSimMeshLoader
