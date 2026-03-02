// Copyright CamSim Contributors. All Rights Reserved.

#include "GameMode/CamSimGameMode.h"
#include "CamSimTest.h"
#include "Camera/CamSimCamera.h"
#include "Environment/CamSimEnvironment.h"
#include "EngineUtils.h"

ACamSimGameMode::ACamSimGameMode()
{
	// No default pawn — the camera actor drives the view directly.
	DefaultPawnClass = nullptr;
}

void ACamSimGameMode::BeginPlay()
{
	Super::BeginPlay();

	// If ACamSimCamera was placed in the level by the designer, use it as-is.
	for (TActorIterator<ACamSimCamera> It(GetWorld()); It; ++It)
	{
		UE_LOG(LogCamSim, Log, TEXT("ACamSimGameMode: found existing ACamSimCamera '%s' in level"),
			*(*It)->GetName());
		return;
	}

	// Nothing placed — spawn one at the origin. The CIGI receiver will move it
	// to the correct geospatial position on the first received Entity Control packet.
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ACamSimCamera* Camera = GetWorld()->SpawnActor<ACamSimCamera>(
		ACamSimCamera::StaticClass(), FTransform::Identity, Params);

	if (Camera)
	{
		UE_LOG(LogCamSim, Log, TEXT("ACamSimGameMode: spawned ACamSimCamera"));
	}
	else
	{
		UE_LOG(LogCamSim, Error, TEXT("ACamSimGameMode: SpawnActor<ACamSimCamera> FAILED"));
	}

	// Spawn environment actor if not already in the level
	for (TActorIterator<ACamSimEnvironment> It(GetWorld()); It; ++It)
	{
		UE_LOG(LogCamSim, Log, TEXT("ACamSimGameMode: found existing ACamSimEnvironment '%s' in level"),
			*(*It)->GetName());
		return;
	}

	FActorSpawnParameters EnvParams;
	EnvParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ACamSimEnvironment* Env = GetWorld()->SpawnActor<ACamSimEnvironment>(
		ACamSimEnvironment::StaticClass(), FTransform::Identity, EnvParams);

	if (Env)
	{
		UE_LOG(LogCamSim, Log, TEXT("ACamSimGameMode: spawned ACamSimEnvironment"));
	}
	else
	{
		UE_LOG(LogCamSim, Error, TEXT("ACamSimGameMode: SpawnActor<ACamSimEnvironment> FAILED"));
	}
}
