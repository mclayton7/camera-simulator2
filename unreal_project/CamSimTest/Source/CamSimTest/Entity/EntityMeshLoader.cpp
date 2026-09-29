// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/EntityMeshLoader.h"

#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/SoftObjectPath.h"
#include "glTFRuntimeFunctionLibrary.h"
#include "glTFRuntimeAsset.h"

namespace
{
	// Resolve a config-relative glTF path to an absolute filesystem path.
	// Config paths are relative to {repo_root}/entities/.
	// FPaths::ProjectDir() is {repo_root}/unreal_project/CamSimTest/.
	FString ResolveGltfPath(const FString& RelPath)
	{
		FString Base = FPaths::Combine(FPaths::ProjectDir(), TEXT("../../entities/"));
		return FPaths::ConvertRelativePathToFull(Base + RelPath);
	}
} // namespace

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
			FString AbsPath = ResolveGltfPath(Path);
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
			FString AbsPath = ResolveGltfPath(Path);
			UglTFRuntimeAsset* Asset = UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilename(
				AbsPath, false, FglTFRuntimeConfig());
			if (!Asset) return nullptr;
			return Asset->LoadSkeletalMeshRecursive(TEXT(""), {}, FglTFRuntimeSkeletalMeshConfig());
		}
		return Cast<USkeletalMesh>(FSoftObjectPath(Path).TryLoad());
	}
} // namespace CamSimMeshLoader
