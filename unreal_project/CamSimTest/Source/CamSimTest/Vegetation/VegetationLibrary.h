// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** One tree type of the vegetation library (REALISM R4 slice, docs/vegetation.md). */
struct FVegetationType
{
	FString Name;
	FString GltfPath;       // absolute; empty when procedural
	float   HeightM = 0.0f; // the model's real height
	FString ThermalClass;   // informational in the slice (the pixel's class comes from the land cover)
	FString Licence;
	FString SourceUrl;
	FString Sha256;         // lowercase hex; empty when procedural
	bool    bProcedural = false;
};

struct FVegetationLibrary
{
	TArray<FVegetationType> Types;
};

namespace CamSimVegetation
{
	/** library.json ("camsim-vegetation-1"). Dir makes gltf paths absolute. Unknown keys are ignored. */
	CAMSIMTEST_API bool ParseLibrary(const FString& Json, const FString& Dir, FVegetationLibrary& Out, FString& OutError);
	/** "" -> Content/NonUFS/Vegetation; relative -> project dir; absolute kept. */
	CAMSIMTEST_API FString ResolveLibraryDir(const FString& ConfigDir);
	/** Reads <Dir>/library.json and checks every non-procedural file's sha256 (an LFS pointer gets the git lfs pull hint). */
	CAMSIMTEST_API bool LoadLibrary(const FString& Dir, FVegetationLibrary& Out, FString& OutError);
	CAMSIMTEST_API FString Sha256Hex(TConstArrayView<uint8> Bytes);
}
