// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UCamSimSubsystem;
class UWorld;
class AActor;
struct FOceanWaveCommand;
struct FMaritimeSurfaceCommand;

/**
 * Draws the subsystem's FOceanSurface (ROADMAP 2.6) and applies CIGI ocean
 * commands to it. Lives inside ACamSimEnvironment. Rendering arrives in Task 9.
 */
class FOceanManager
{
public:
	void Init(UWorld* World, AActor* Owner, UCamSimSubsystem* Subsystem);
	void Tick();
	void ApplyWave(const FOceanWaveCommand& Cmd);
	void ApplyMaritimeSurface(const FMaritimeSurfaceCommand& Cmd);

private:
	UCamSimSubsystem* Subsystem = nullptr;
	bool bWarnedScopedWave = false, bWarnedWaveId = false, bWarnedScopedMaritime = false;
};
