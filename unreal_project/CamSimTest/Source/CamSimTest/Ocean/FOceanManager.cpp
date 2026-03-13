// Copyright CamSim Contributors. All Rights Reserved.

#include "Ocean/FOceanManager.h"
#include "Ocean/FBeaufortTable.h"
#include "Subsystem/CamSimSubsystem.h"
#include "Entity/CamSimEntityManager.h"
#include "Engine/World.h"
#include "Engine/PostProcessVolume.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"

void FOceanManager::Init(UWorld* World, AActor* OuterActor,
                          UCamSimSubsystem* Subsystem,
                          const FCamSimConfig::FPhase19Config& Cfg)
{
	Config       = Cfg;
	OceanEnabled = Cfg.bOceanEnabled;

	if (!OceanEnabled) return;

	// --- Create ocean plane mesh component (registered to OuterActor, GC-safe) ---
	UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(
		OuterActor, TEXT("OceanPlaneMesh"));
	MeshComp->RegisterComponent();

	// Use the engine built-in plane (200km scaled via component transform)
	UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(
		nullptr, TEXT("/Engine/BasicShapes/Plane"));
	if (PlaneMesh)
	{
		MeshComp->SetStaticMesh(PlaneMesh);
		// Scale to 200km x 200km (plane asset is 100x100 UE units = 1m x 1m)
		MeshComp->SetWorldScale3D(FVector(200000.0f, 200000.0f, 1.0f));
	}
	MeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MeshComp->SetCastShadow(false);

	// --- Create sky light component for reflection capture ---
	USkyLightComponent* SkyComp = NewObject<USkyLightComponent>(
		OuterActor, TEXT("OceanSkyLight"));
	SkyComp->SourceType              = SLS_CapturedScene;
	SkyComp->bRealTimeCapture = false;
	SkyComp->RegisterComponent();

	// --- Initialise ocean surface ---
	GerstnerSurface.Init(Cfg.OceanMaterialPath, MeshComp, SkyComp, World);
	GerstnerSurface.SetEnabled(true);

	// --- Apply startup Beaufort state ---
	const FBeaufortEntry Entry = FBeaufortTable::Sample(Cfg.BeaufortState);
	GerstnerSurface.SetWaveParams(Entry.WaveHtM, Entry.WaveLenM,
	                               Cfg.WaveAmplitudeScale, Cfg.WaveFrequencyScale,
	                               Entry.Choppiness);

	// --- SSR reflections ---
	if (Cfg.bOceanReflectionsEnabled)
	{
		EnableSSR(World, Cfg.SSRIntensity);
	}

	// --- Inject IOceanSurface* into entity manager ---
	// NOTE: FCamSimEntityManager::SetOceanSurface() is added in Task 13.
	// The call below will link correctly once that task is complete.
	if (Subsystem)
	{
		if (FCamSimEntityManager* EM = Subsystem->GetEntityManager())
		{
			EM->SetOceanSurface(GetOceanSurface());
		}
	}
}

void FOceanManager::Tick(float DeltaTime, const FVector& CameraWorldLocation)
{
	if (!OceanEnabled) return;
	GerstnerSurface.RepositionToCamera(CameraWorldLocation);
	GerstnerSurface.Tick(DeltaTime);
}

void FOceanManager::ApplyWaveState(const FCigiWaveState& State)
{
	if (!OceanEnabled || !State.bEnabled) return;
	// CIGI overrides Beaufort table directly with physical parameters
	GerstnerSurface.SetWaveParams(State.WaveHtM, State.WaveLenM,
	                               Config.WaveAmplitudeScale, Config.WaveFrequencyScale,
	                               Config.WaveChoppiness);
}

void FOceanManager::OnAtmosphereChanged()
{
	if (!OceanEnabled) return;
	GerstnerSurface.RecaptureSky();
}

void FOceanManager::EnableSSR(UWorld* World, float Intensity)
{
	for (TActorIterator<APostProcessVolume> It(World); It; ++It)
	{
		PostProcessVolume = *It;
		PostProcessVolume->Settings.bOverride_ScreenSpaceReflectionIntensity = true;
		PostProcessVolume->Settings.ScreenSpaceReflectionIntensity = Intensity;
		break; // Use first (global) PPV
	}
}
