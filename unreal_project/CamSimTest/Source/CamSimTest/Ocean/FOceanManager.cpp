// Copyright CamSim Contributors. All Rights Reserved.

#include "Ocean/FOceanManager.h"
#include "Sim/Commands.h"
#include "Ocean/OceanCommands.h"
#include "Ocean/OceanSurface.h"
#include "Ocean/OceanWaves.h"
#include "Subsystem/CamSimSubsystem.h"
#include "Geospatial/CamSimGeospatialProvider.h"
#include "Geospatial/CigiFrames.h"
#include "Geospatial/EcefFrames.h"
#include "Metadata/CamSimTelemetry.h"
#include "CamSimTest.h"

#include "CesiumGeoreference.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Engine/World.h"

namespace CamSimOcean
{
	void WriteMpc(UWorld* World, UMaterialParameterCollection* Mpc, const FOceanSurface& Ocean,
		const FVector& ComponentWorld, const FMatrix& EcefToUe)
	{
		if (!World || !Mpc) return;
		const FOceanWaves& W = Ocean.GetWaves();
		for (int32 i = 0; i < FOceanWaves::MaxWaves; ++i)
		{
			FLinearColor Wave(0, 0, 0, 0), Dir(0, 0, 0, 0);
			if (i < W.GetWaves().Num())
			{
				const FVector2D D = W.TravelDir(i);
				Wave = FLinearColor(float(W.WaveNumber(i)), float(W.Amplitude(i)), float(W.GetWaves()[i].Steepness), float(W.Phase(i)));
				Dir  = FLinearColor(float(D.X), float(D.Y), 0.f, 0.f);
			}
			UKismetMaterialLibrary::SetVectorParameterValue(World, Mpc, FName(*FString::Printf(TEXT("Wave%d"), i)), Wave);
			UKismetMaterialLibrary::SetVectorParameterValue(World, Mpc, FName(*FString::Printf(TEXT("Dir%d"), i)), Dir);
		}
		// The GPU plane frame is the anchor at altitude 0 (as FOceanWaves::SetAnchor), in UE world axes.
		const FVector AnchorWorld = EcefToUe.TransformPosition(W.GetAnchorEcef());
		const FVector ToAnchor = ComponentWorld - AnchorWorld;
		const FVector AxisN = EcefToUe.TransformVector(W.GetAxisNorthEcef()).GetSafeNormal();
		const FVector AxisE = EcefToUe.TransformVector(W.GetAxisEastEcef()).GetSafeNormal();
		const FVector AxisU = EcefToUe.TransformVector(W.GetAxisUpEcef()).GetSafeNormal();
		auto V3 = [](const FVector& V) { return FLinearColor(float(V.X), float(V.Y), float(V.Z), 0.f); };
		UKismetMaterialLibrary::SetVectorParameterValue(World, Mpc, TEXT("ComponentToAnchor"), V3(ToAnchor));
		UKismetMaterialLibrary::SetVectorParameterValue(World, Mpc, TEXT("AxisN"), V3(AxisN));
		UKismetMaterialLibrary::SetVectorParameterValue(World, Mpc, TEXT("AxisE"), V3(AxisE));
		UKismetMaterialLibrary::SetVectorParameterValue(World, Mpc, TEXT("AxisU"), V3(AxisU));
		const double Clarity = Ocean.GetClarity();
		UKismetMaterialLibrary::SetVectorParameterValue(World, Mpc, TEXT("Water"),
			FLinearColor(float(FMath::Lerp(2.0, 0.5, Clarity)), float(FMath::Lerp(2.0, 0.7, Clarity)), 0.3f, 0.f));
	}
}

namespace
{
	/** "/Game/X/Y" → "/Game/X/Y.Y" (LoadObject wants the object path). */
	FString ToObjectPath(const FString& Path)
	{
		if (Path.Contains(TEXT("."))) return Path;
		return Path + TEXT(".") + FPaths::GetBaseFilename(Path);
	}
}

void FOceanManager::Init(UWorld* InWorld, AActor* Owner, UCamSimSubsystem* InSubsystem)
{
	Subsystem = InSubsystem;
	World = InWorld;
	if (!Subsystem || !Subsystem->GetOceanSurface() || !InWorld) return;   // ocean off: nothing drawn
	Cfg = Subsystem->GetConfig().Ocean;

	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *ToObjectPath(Cfg.MaterialPath));
	UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/Ocean/MPC_Ocean.MPC_Ocean"));
	if (!Material || !Collection)
	{
		UE_LOG(LogCamSim, Warning, TEXT("Ocean: M_Ocean/MPC_Ocean missing (%s) — run scripts/ocean/make_ocean_material.sh; boats still float"), *Cfg.MaterialPath);
		return;
	}
	Mpc = Collection;
	Georeference = ACesiumGeoreference::GetDefaultGeoreference(InWorld);
	Mesh.Init(Owner, Material);
	UE_LOG(LogCamSim, Log, TEXT("Ocean: drawing the sea with %s (max radius %.0f km)"), *Material->GetPathName(), Cfg.MaxRadiusKm);
}

void FOceanManager::CheckEcefUnitsOnce(const FMatrix& EcefToUe)
{
	if (bUnitsChecked) return;
	bUnitsChecked = true;
	const FOceanWaves& W = Subsystem->GetOceanSurface()->GetWaves();
	const FCamSimGeospatialProvider* Geo = Subsystem->GetGeospatialProvider();
	FVector Ref;
	if (!Geo || !Geo->GeoToWorld(World.Get(), W.GetAnchorLat(), W.GetAnchorLon(), 0.0, Ref))
	{
		UE_LOG(LogCamSim, Warning, TEXT("Ocean: ECEF->UE unit check skipped (no geospatial provider)"));
		return;
	}
	const FVector Mine = EcefToUe.TransformPosition(CamSimFrames::GeodeticToEcef(W.GetAnchorLat(), W.GetAnchorLon(), 0.0));
	const double ErrCm = FVector::Dist(Mine, Ref);
	if (ErrCm <= 1.0)
	{
		UE_LOG(LogCamSim, Log, TEXT("Ocean: ECEF->UE check at anchor (%.5f, %.5f): Cesium matrix vs GeoToWorld differ by %.3f cm (OK: metres in, cm out)"),
			W.GetAnchorLat(), W.GetAnchorLon(), ErrCm);
	}
	else
	{
		UE_LOG(LogCamSim, Error, TEXT("Ocean: ECEF->UE check at anchor (%.5f, %.5f): Cesium matrix vs GeoToWorld differ by %.3f cm (MISMATCH: sea misplaced)"),
			W.GetAnchorLat(), W.GetAnchorLon(), ErrCm);
	}
}

void FOceanManager::Tick(const FCamSimTelemetry* Cam)
{
	FOceanSurface* Ocean = Subsystem ? Subsystem->GetOceanSurface() : nullptr;
	if (!Ocean || !Cam || !Georeference.IsValid() || !Mesh.IsValid() || !Mpc.IsValid()) return;
	if (!FMath::IsFinite(Cam->Latitude) || !FMath::IsFinite(Cam->Longitude)) return;

	// Teleport: the nadir moved > 5 km since the last tick (no camera flies 150 km/s).
	const FVector Hop = CamSimFrames::GeodeticDeltaToNeu(LastNadirLat, LastNadirLon, 0.0, Cam->Latitude, Cam->Longitude, 0.0);
	const bool bTeleport = bHasNadir && Hop.X * Hop.X + Hop.Y * Hop.Y > FMath::Square(5000.0);
	LastNadirLat = Cam->Latitude; LastNadirLon = Cam->Longitude; bHasNadir = true;

	double CLat, CLon;
	CamSimOcean::ChooseCentre(Cam->Latitude, Cam->Longitude, Cam->FrameCenterLat, Cam->FrameCenterLon,
		Cam->FrameCenterLat != 0.0 || Cam->FrameCenterLon != 0.0, CLat, CLon);
	const FOceanWaves& W = Ocean->GetWaves();
	if (CamSimOcean::NeedsReanchor(W.HasAnchor(), W.GetAnchorLat(), W.GetAnchorLon(), CLat, CLon, bTeleport))
	{
		Ocean->SetAnchor(CLat, CLon);
		UE_LOG(LogCamSim, Log, TEXT("Ocean: wave anchor set to %.5f %.5f"), CLat, CLon);
	}

	const FMatrix EcefToUe = Georeference->ComputeEarthCenteredEarthFixedToUnrealTransformation();
	CheckEcefUnitsOnce(EcefToUe);

	const double SeaAtNadir = Ocean->SeaLevelM(Cam->Latitude, Cam->Longitude).Get(0.0);
	const FVector CN = CamSimFrames::GeodeticDeltaToNeu(Cam->Latitude, Cam->Longitude, 0.0, CLat, CLon, 0.0);
	const double R = CamSimOcean::HorizonRadiusM(FMath::Sqrt(CN.X * CN.X + CN.Y * CN.Y), Cam->Altitude - SeaAtNadir, Cfg.MaxRadiusKm);
	// A Cesium origin shift moves the UE frame under the (geographic) mesh: rebuild in the new frame.
	const bool bFrameMoved = Last.bHasMesh && !EcefToUe.Equals(LastEcefToUe, 1e-3);
	if (bTeleport || bFrameMoved || CamSimOcean::NeedsRebuild(Last, CLat, CLon, R))
	{
		auto GeoToWorld = [&EcefToUe](double Lat, double Lon, double Alt)
			{ return EcefToUe.TransformPosition(CamSimFrames::GeodeticToEcef(Lat, Lon, Alt)); };
		const double T0 = FPlatformTime::Seconds();
		CamSimOcean::FOceanMeshData Data;
		if (CamSimOcean::BuildOceanMesh(CLat, CLon, R, *Ocean, GeoToWorld, Data))
		{
			Mesh.Upload(Data);
			Last = { CLat, CLon, R, true };
			LastEcefToUe = EcefToUe;
			const double Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
			// Log (not Verbose): the rebuild cost decides whether rebuilding beats translating (ROADMAP 2.6).
			UE_LOG(LogCamSim, Log, TEXT("Ocean: mesh rebuilt in %.1f ms (R %.1f km, centre %.5f %.5f)%s"),
				Ms, R / 1000.0, CLat, CLon, Ms > 10.0 ? TEXT(" — over the 10 ms budget") : TEXT(""));
		}
	}

	// WPO moves vertices by up to ~sum(a) vertically and sum(Q a) sideways: keep them inside the bounds.
	double SumA = 0.0;
	for (int32 i = 0; i < W.GetWaves().Num(); ++i) SumA += W.Amplitude(i);
	Mesh.SetDisplacementPadding(3.0 * SumA + 10.0);

	CamSimOcean::WriteMpc(World.Get(), Mpc.Get(), *Ocean, Mesh.GetWorldLocation(), EcefToUe);
}

void FOceanManager::ApplyWave(const FOceanWaveCommand& Cmd)
{
	FOceanSurface* Ocean = Subsystem ? Subsystem->GetOceanSurface() : nullptr;
	if (!Ocean) return;
	if (Cmd.Scope != FWeatherCommand::EScope::Global)
	{
		if (!bWarnedScopedWave) { bWarnedScopedWave = true; UE_LOG(LogCamSim, Warning, TEXT("Ocean: regional/entity Wave Control ignored (Global only)")); }
		return;
	}
	if (Cmd.WaveId >= FOceanWaves::MaxWaves)
	{
		if (!bWarnedWaveId) { bWarnedWaveId = true; UE_LOG(LogCamSim, Warning, TEXT("Ocean: Wave ID %u ignored (0-3 supported)"), Cmd.WaveId); }
		return;
	}
	Ocean->SetHostWave(Cmd.WaveId, CamSimOcean::ToOceanWave(Cmd));
}

void FOceanManager::ApplyMaritimeSurface(const FMaritimeSurfaceCommand& Cmd)
{
	FOceanSurface* Ocean = Subsystem ? Subsystem->GetOceanSurface() : nullptr;
	if (!Ocean || !Cmd.bEnabled) return;
	if (Cmd.Scope != FWeatherCommand::EScope::Global)
	{
		if (!bWarnedScopedMaritime) { bWarnedScopedMaritime = true; UE_LOG(LogCamSim, Warning, TEXT("Ocean: regional/entity Maritime Surface Conditions ignored (Global only)")); }
		return;
	}
	Ocean->SetTideOffsetM(Cmd.SurfaceHeightM);
	Ocean->SetClarity(Cmd.Clarity);
	Ocean->SetWaterTempC(Cmd.WaterTempC);
}
