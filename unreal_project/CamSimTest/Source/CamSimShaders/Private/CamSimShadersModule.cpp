// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

/**
 * Global compute shaders for the GPU sensor model (ROADMAP 3B). Loads at
 * PostConfigInit, before the global shader map is compiled, and maps the
 * virtual directory /CamSim to <project>/Shaders.
 */
class FCamSimShadersModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Shaders")));
		if (!AllShaderSourceDirectoryMappings().Contains(TEXT("/CamSim")))
		{
			AddShaderSourceDirectoryMapping(TEXT("/CamSim"), Dir);
		}
	}
};

IMPLEMENT_MODULE(FCamSimShadersModule, CamSimShaders)
