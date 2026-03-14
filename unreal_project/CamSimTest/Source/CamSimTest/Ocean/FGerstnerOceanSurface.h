// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Ocean/IOceanSurface.h"

class UStaticMeshComponent;
class USkyLightComponent;
class UMaterialParameterCollection;
class UWorld;

/**
 * GPU Gerstner ocean surface implementation.
 *
 * Writes wave parameters to a UMaterialParameterCollection each tick (drives
 * World Position Offset in the ocean Material). Also provides a CPU analytic
 * GetSurfaceHeightAt() for vessel pitch/roll/heave sampling (19C).
 *
 * Owned by FOceanManager. Component pointers are non-owning (GC managed by ACamSimEnvironment).
 */
class FGerstnerOceanSurface : public IOceanSurface
{
public:
	FGerstnerOceanSurface() = default;
	virtual ~FGerstnerOceanSurface() override = default;

	/**
	 * Bind to existing components (created by FOceanManager with OuterActor for GC safety).
	 * Loads the MPC from the soft path.
	 */
	void Init(const FString& MaterialPath,
	          UStaticMeshComponent* InOceanMesh,
	          USkyLightComponent*   InOceanSky,
	          UWorld*               InWorld);

	// --- IOceanSurface interface ---
	virtual void  SetWaveParams(float WaveHtM, float WaveLenM,
	                            float AmplitudeScale, float FrequencyScale,
	                            float InChoppiness) override;
	virtual void  Tick(float DeltaTime) override;
	virtual float GetSurfaceHeightAt(FVector2D WorldXY) const override;
	virtual void  SetEnabled(bool bEnabled) override;

	/** Recapture sky light — call when atmosphere changes. */
	void RecaptureSky();

	/** Snap ocean plane XY to camera position each tick. */
	void RepositionToCamera(const FVector& CameraWorldLocation);

private:
	// Non-owning — GC managed by ACamSimEnvironment (the outer actor)
	UStaticMeshComponent*         OceanMesh = nullptr;
	USkyLightComponent*           OceanSky  = nullptr;
	UMaterialParameterCollection* MPC       = nullptr;
	UWorld*                       World     = nullptr;

	// Wave state (CPU mirror of GPU MPC values)
	float AmplitudeCm  = 0.0f;  // half peak-to-trough in UE units (cm)
	float WaveNumber   = 0.0f;  // k = 2π/λ (λ in cm)
	float AngularFreq  = 0.0f;  // ω = sqrt(g·k), g = 981 cm/s²
	float Choppiness   = 0.5f;
	float ElapsedTime  = 0.0f;
	float PhaseOffset  = 0.0f;  // AngularFreq * ElapsedTime, cached each Tick
	bool  bEnabled     = false;

	void WriteMPC() const;
};
