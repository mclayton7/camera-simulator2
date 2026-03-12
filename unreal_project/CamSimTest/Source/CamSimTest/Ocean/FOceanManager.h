// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Ocean/IOceanSurface.h"
#include "Ocean/FGerstnerOceanSurface.h"
#include "CIGI/CigiPacketTypes.h"
#include "Config/CamSimConfig.h"

class UCamSimSubsystem;
class APostProcessVolume;

/**
 * Owns the ocean surface implementation and drives it each tick.
 * Lives inside ACamSimEnvironment.
 *
 * Init() creates UObject components (registered to OuterActor for GC safety),
 * translates startup Beaufort config to wave params, enables SSR on the
 * PostProcess Volume, and injects IOceanSurface* into FCamSimEntityManager.
 */
class FOceanManager
{
public:
	FOceanManager() = default;
	~FOceanManager() = default;

	/**
	 * Create rendering components, apply startup config, wire entity manager.
	 * Must be called from ACamSimEnvironment::BeginPlay().
	 */
	void Init(UWorld* World, AActor* OuterActor, UCamSimSubsystem* Subsystem,
	          const FCamSimConfig::FPhase19Config& Cfg);

	/** Called from ACamSimEnvironment::Tick(). Applies queued CIGI wave states and ticks surface. */
	void Tick(float DeltaTime, const FVector& CameraWorldLocation);

	/** Apply a CIGI Wave Control packet — called from ACamSimEnvironment::Tick() drain loop. */
	void ApplyWaveState(const FCigiWaveState& State);

	/** Non-owning pointer to the ocean surface (for IOceanSurface* injection into entity manager). */
	IOceanSurface* GetOceanSurface() { return OceanEnabled ? &GerstnerSurface : nullptr; }

	/** Called by ACamSimEnvironment::OnAtmosphereChanged() to refresh sky reflections. */
	void OnAtmosphereChanged();

private:
	FGerstnerOceanSurface GerstnerSurface;
	FCamSimConfig::FPhase19Config Config;
	bool OceanEnabled = false;

	// Cached for SSR enable/disable
	TWeakObjectPtr<APostProcessVolume> PostProcessVolume;

	void EnableSSR(UWorld* World, float Intensity);
};
