// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Paths.h"

namespace CamSimEntityPaths
{
	// Absolute path of an asset given relative to the entities/ directory.
	// A packaged build carries it at <ProjectDir>/entities/
	// (scripts/package_for_docker.sh stages it there); from the editor it is
	// {repo_root}/entities/, ProjectDir being {repo_root}/unreal_project/CamSimTest/.
	inline FString Resolve(const FString& RelPath)
	{
		const FString Packaged = FPaths::Combine(FPaths::ProjectDir(), TEXT("entities/"));
		const FString Base = FPaths::DirectoryExists(Packaged)
			? Packaged
			: FPaths::Combine(FPaths::ProjectDir(), TEXT("../../entities/"));
		return FPaths::ConvertRelativePathToFull(Base + RelPath);
	}
} // namespace CamSimEntityPaths
