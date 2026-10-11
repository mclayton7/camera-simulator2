// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Cesium3DTileset.h"
#include "CesiumSampleHeightResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Entity/EntityMeshLoader.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Camera/CamSimCamera.h"
#include "Subsystem/CamSimSubsystem.h"
#include "Vegetation/VegetationCellActor.h"
#include "CamSimTest.h"

namespace
{
	void LogInstancedUsage(UStaticMesh* Mesh)
	{
		for (const FStaticMaterial& M : Mesh->GetStaticMaterials())
		{
			const UMaterial* Base = M.MaterialInterface ? M.MaterialInterface->GetMaterial() : nullptr;
			UE_LOG(LogCamSim, Log, TEXT("Vegetation probe: material '%s' (base '%s') instanced usage = %d"),
				M.MaterialInterface ? *M.MaterialInterface->GetPathName() : TEXT("<none>"),
				Base ? *Base->GetPathName() : TEXT("<none>"),
				Base ? static_cast<int32>(Base->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes)) : -1);
		}
	}

	void Probe(const TArray<FString>& Args, UWorld* World)
	{
		if (Args.Num() != 1 || !World) { UE_LOG(LogCamSim, Warning, TEXT("usage: camsim.Vegetation.Probe <abs .glb>")); return; }
		UStaticMesh* Mesh = CamSimMeshLoader::LoadStaticMesh(Args[0]);
		if (!Mesh) { UE_LOG(LogCamSim, Error, TEXT("Vegetation probe: could not load '%s'"), *Args[0]); return; }
		UE_LOG(LogCamSim, Log, TEXT("Vegetation probe: '%s' %d LODs, bounds %s"), *Args[0], Mesh->GetNumLODs(),
			*Mesh->GetBounds().BoxExtent.ToString());
		LogInstancedUsage(Mesh);
		UCamSimSubsystem* Sub = World->GetGameInstance() ? World->GetGameInstance()->GetSubsystem<UCamSimSubsystem>() : nullptr;
		ACamSimCamera* Cam = Sub ? Sub->GetCamera() : nullptr;
		ACesium3DTileset* Terrain = (Sub && Sub->GetCachedTilesets().Num() > 0) ? Sub->GetCachedTilesets()[0].Get() : nullptr; // Task 9 replaces this with Sub->GetTerrainTileset()
		if (!Cam || !Terrain) { UE_LOG(LogCamSim, Error, TEXT("Vegetation probe: no camera or terrain tileset")); return; }
		const double Lat = Cam->GetTelemetry().Latitude, Lon = Cam->GetTelemetry().Longitude;
		TWeakObjectPtr<UWorld> WeakWorld(World);
		TStrongObjectPtr<UStaticMesh> Keep(Mesh);
		Terrain->SampleHeightMostDetailed({ FVector(Lon, Lat, 0.0) }, FCesiumSampleHeightMostDetailedCallback::CreateLambda(
			[WeakWorld, Keep, Lat, Lon](ACesium3DTileset*, const TArray<FCesiumSampleHeightResult>& R, const TArray<FString>& W)
			{
				UWorld* Wd = WeakWorld.Get();
				if (!Wd || R.Num() != 1 || !R[0].SampleSuccess) { UE_LOG(LogCamSim, Error, TEXT("Vegetation probe: height sample failed")); return; }
				TArray<FTransform> Xf;
				for (int32 J = 0; J < 10; ++J)
					for (int32 I = 0; I < 10; ++I)
						Xf.Add(FTransform(FRotator(0.0, 37.0 * (I + 10 * J), 0.0), FVector((I - 4.5) * 1500.0, (J - 4.5) * 1500.0, 0.0)));
				ACamSimVegetationCell* Cell = Wd->SpawnActor<ACamSimVegetationCell>();
				Cell->Init(Lat, Lon, R[0].LongitudeLatitudeHeight.Z, Keep.Get(), Xf, true, 5000.0f);
				UE_LOG(LogCamSim, Log, TEXT("Vegetation probe: spawned %d instances at (%.6f, %.6f, %.1f m)"),
					Cell->GetInstanceCount(), Lat, Lon, R[0].LongitudeLatitudeHeight.Z);
			}));
	}

	FAutoConsoleCommandWithWorldAndArgs GVegetationProbe(TEXT("camsim.Vegetation.Probe"),
		TEXT("Spawn a 10x10 grid of a .glb at the camera nadir and log its materials' instanced usage (REALISM R4)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) { Probe(Args, World); }));
}
