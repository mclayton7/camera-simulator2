// Copyright CamSim Contributors. All Rights Reserved.

#include "Ocean/FGerstnerOceanSurface.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkyLightComponent.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Engine/World.h"
#include "Kismet/KismetMaterialLibrary.h"

void FGerstnerOceanSurface::Init(const FString& MaterialPath,
                                  UStaticMeshComponent* InOceanMesh,
                                  USkyLightComponent*   InOceanSky,
                                  UWorld*               InWorld)
{
	OceanMesh = InOceanMesh;
	OceanSky  = InOceanSky;

	if (!MaterialPath.IsEmpty())
	{
		UMaterialParameterCollection* MPC = LoadObject<UMaterialParameterCollection>(
			nullptr, *MaterialPath);
		if (MPC && InWorld)
		{
			MPCInstance = MPC;
		}
	}
}

void FGerstnerOceanSurface::SetWaveParams(float WaveHtM, float WaveLenM,
                                           float AmplScale, float FreqScale,
                                           float Choppiness)
{
	Choppiness_ = FMath::Clamp(Choppiness, 0.0f, 1.0f);

	if (WaveHtM <= 0.0f || WaveLenM <= 0.0f)
	{
		AmplitudeCm = 0.0f;
		WaveNumber  = 0.0f;
		AngularFreq = 0.0f;
	}
	else
	{
		// Amplitude in cm, halved (half peak-to-trough)
		AmplitudeCm = WaveHtM * 100.0f * 0.5f * AmplScale;

		// Wave number: k = 2π / λ (λ in cm)
		const float LambdaCm = WaveLenM * 100.0f * FMath::Max(FreqScale, 0.01f);
		WaveNumber = (2.0f * PI) / LambdaCm;

		// Angular frequency from deep-water dispersion: ω = sqrt(g·k), g = 981 cm/s²
		AngularFreq = FMath::Sqrt(981.0f * WaveNumber);
	}

	WriteMPC();
}

void FGerstnerOceanSurface::Tick(float DeltaTime)
{
	if (!bEnabled_) return;
	ElapsedTime += DeltaTime;
	WriteMPC();
}

float FGerstnerOceanSurface::GetSurfaceHeightAt(FVector2D WorldXY) const
{
	if (AmplitudeCm <= 0.0f || WaveNumber <= 0.0f) return 0.0f;

	// Single dominant wave, propagating along +X axis (fixed direction, Sprint 1)
	// Z = A · cos(k·x − ω·t)
	const float Phase = WaveNumber * WorldXY.X - AngularFreq * ElapsedTime;
	return AmplitudeCm * FMath::Cos(Phase);
}

void FGerstnerOceanSurface::SetEnabled(bool bInEnabled)
{
	bEnabled_ = bInEnabled;
	if (OceanMesh)
	{
		OceanMesh->SetVisibility(bInEnabled);
	}
}

void FGerstnerOceanSurface::RecaptureSky()
{
	if (OceanSky)
	{
		OceanSky->RecaptureSky();
	}
}

void FGerstnerOceanSurface::RepositionToCamera(const FVector& CameraWorldLocation)
{
	if (OceanMesh)
	{
		// Snap XY to camera, hold Z at sea level (0)
		FVector NewLoc = CameraWorldLocation;
		NewLoc.Z = 0.0f;
		OceanMesh->SetWorldLocation(NewLoc);
	}
}

void FGerstnerOceanSurface::WriteMPC() const
{
	if (!MPCInstance) return;

	MPCInstance->SetScalarParameterValue(FName("Amplitude"),  AmplitudeCm);
	MPCInstance->SetScalarParameterValue(FName("Frequency"),  WaveNumber);
	MPCInstance->SetScalarParameterValue(FName("Choppiness"), Choppiness_);
	MPCInstance->SetScalarParameterValue(FName("Time"),       ElapsedTime);
}
