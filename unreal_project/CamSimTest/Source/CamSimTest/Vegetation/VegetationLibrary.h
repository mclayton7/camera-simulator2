// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** What a library type is scattered as (REALISM R4): trees on the tree grid (canopy-driven), shrubs on the shrub grid. */
enum class EVegRole : uint8 { Tree, Shrub };

/** One type of the vegetation library (REALISM R4, docs/vegetation.md); camsim-vegetation-1 types are trees. */
struct FVegetationType
{
	FString Name;
	EVegRole Role = EVegRole::Tree;
	float   CrownRadiusM = 0.0f;   // canopy cover -> density; default 0.3 x height (tree), 0.5 x height (shrub)
	FString FarGltfPath;           // absolute; the far-tier mesh (trees only), empty = not drawn far
	FString FarSha256;
	FString GltfPath;       // absolute; empty when procedural
	float   HeightM = 0.0f; // the model's real height
	FString ThermalClass;   // informational in the slice (the pixel's class comes from the land cover)
	FString Licence;
	FString SourceUrl;
	FString Sha256;         // lowercase hex; empty when procedural
	bool    bProcedural = false;

	bool HasFarMesh() const { return !FarGltfPath.IsEmpty(); }
};

struct FVegetationLibrary
{
	TArray<FVegetationType> Types;
};

namespace CamSimVegetation
{
	/** library.json ("camsim-vegetation-1" or "-2": roles, crown radius, far meshes). Dir makes gltf paths absolute.
	 *  Unknown keys are ignored; duplicate names, unknown roles and a shrub with a far mesh are refused. */
	CAMSIMTEST_API bool ParseLibrary(const FString& Json, const FString& Dir, FVegetationLibrary& Out, FString& OutError);
	/** "" -> Content/NonUFS/Vegetation; relative -> project dir; absolute kept. */
	CAMSIMTEST_API FString ResolveLibraryDir(const FString& ConfigDir);
	/** Reads <Dir>/library.json and checks every non-procedural file's sha256 (an LFS pointer gets the git lfs pull hint). */
	CAMSIMTEST_API bool LoadLibrary(const FString& Dir, FVegetationLibrary& Out, FString& OutError);
	CAMSIMTEST_API FString Sha256Hex(TConstArrayView<uint8> Bytes);
	/** Library index of the type named Name, or -1. */
	CAMSIMTEST_API int32 FindType(const FVegetationLibrary& Library, const FString& Name);
}
