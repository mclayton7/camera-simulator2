// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/ThermalFrameSources.h"
#include "Thermal/ThermalFrameBuilder.h"
#include "ThermalPass.h"
#include "Thermal/LandCoverWindow.h"
#include "CesiumGeoreference.h"
#include "Environment/CamSimEnvironment.h"
#include "Entity/CamSimEntityManager.h"
#include "Ocean/OceanSurface.h"
#include "Ocean/OceanWaves.h"
#include "Subsystem/CamSimSubsystem.h"
#include "Time/SimClock.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/UObjectIterator.h"

namespace
{
	double Luminance(const FLinearColor& C)
	{
		return 0.2126 * C.R + 0.7152 * C.G + 0.0722 * C.B;
	}

	/** World lookups GatherFrameInputs needs, refreshed every RefreshFrames frames (or on a world change / stale pointer). */
	struct FWorldLookups
	{
		static constexpr uint64 RefreshFrames = 300;

		TWeakObjectPtr<UWorld>                     World;
		TWeakObjectPtr<ACamSimEnvironment>         Environment;
		TWeakObjectPtr<UDirectionalLightComponent> Sun;
		TWeakObjectPtr<USkyAtmosphereComponent>    Sky;
		uint64 RefreshedFrame = 0;
		bool   bValid = false;

		void Refresh(UWorld* InWorld)
		{
			const bool bStale = (Environment.IsStale() || Sun.IsStale() || Sky.IsStale());
			if (bValid && World.Get() == InWorld && !bStale && GFrameCounter - RefreshedFrame < RefreshFrames) return;
			bValid = true;
			World = InWorld;
			RefreshedFrame = GFrameCounter;
			Environment = nullptr;
			Sun = nullptr;
			Sky = nullptr;
			if (!InWorld) return;

			if (TActorIterator<ACamSimEnvironment> It(InWorld); It)
			{
				Environment = *It;
			}
			// Atmosphere sun light (ROADMAP 4A Task 1: CesiumSunSky's DirectionalLight, index 0): visible, used as the
			// atmosphere sun, index 0, brightest.
			UDirectionalLightComponent* Best = nullptr;
			for (TObjectIterator<UDirectionalLightComponent> It; It; ++It)
			{
				if (It->GetWorld() != InWorld || !It->IsVisible() || !It->IsUsedAsAtmosphereSunLight()
					|| It->GetAtmosphereSunLightIndex() != 0) continue;
				if (!Best || It->Intensity > Best->Intensity) Best = *It;
			}
			Sun = Best;
			for (TObjectIterator<USkyAtmosphereComponent> It; It; ++It)
			{
				if (It->GetWorld() == InWorld) { Sky = *It; break; }
			}
		}
	};
}

double CamSimThermal::SunIlluminanceLux(double IntensityLux, const FLinearColor& Color, const FLinearColor& Transmittance,
	double SunElevationDeg)
{
	if (!FMath::IsFinite(IntensityLux) || IntensityLux <= 0.0) return 0.0;
	if (!FMath::IsFinite(SunElevationDeg) || SunElevationDeg <= 0.0) return 0.0;
	const double E = IntensityLux * Luminance(Color) * Luminance(Transmittance) * FMath::Sin(FMath::DegreesToRadians(SunElevationDeg));
	return SanitizeLux(E);
}

double CamSimThermal::MaxWaveAmplitudeM(const FOceanWaves& Waves)
{
	double Sum = 0.0;
	for (int32 i = 0; i < Waves.GetWaves().Num(); ++i)
	{
		Sum += Waves.Amplitude(i);
	}
	return Sum;
}

bool CamSimThermal::SetCameraPose(FThermalFrameInputs& Out, double LatDeg, double LonDeg, double AltHaeM)
{
	if (!FMath::IsFinite(LatDeg) || !FMath::IsFinite(LonDeg) || !FMath::IsFinite(AltHaeM)) return false;
	Out.CamLatDeg = LatDeg;
	Out.CamLonDeg = LonDeg;
	Out.CamAltHaeM = AltHaeM;
	return true;
}

FVector CamSimThermal::SanitizeUpWorld(const FVector& Up)
{
	if (!FMath::IsFinite(Up.X) || !FMath::IsFinite(Up.Y) || !FMath::IsFinite(Up.Z)) return FVector::UpVector;
	const FVector N = Up.GetSafeNormal();
	return N.IsZero() ? FVector::UpVector : N;
}

void CamSimThermal::SetSea(FThermalFrameInputs& Out, TOptional<double> SeaLevelHaeM, double MaxWaveAmpM)
{
	if (!SeaLevelHaeM.IsSet() || !FMath::IsFinite(*SeaLevelHaeM))
	{
		Out.bHasSea = false;
		return;
	}
	Out.bHasSea = true;
	Out.SeaLevelHaeM = *SeaLevelHaeM;
	Out.MaxWaveAmplitudeM = (FMath::IsFinite(MaxWaveAmpM) && MaxWaveAmpM > 0.0) ? MaxWaveAmpM : 0.0;
}

double CamSimThermal::SanitizeLux(double Lux)
{
	return (FMath::IsFinite(Lux) && Lux > 0.0) ? Lux : 0.0;
}

void CamSimThermal::GatherFrameInputs(UWorld* World, const UCamSimSubsystem& Subsystem, double CamLatDeg, double CamLonDeg,
	double CamAltHaeM, const FVector& UpWorld, FThermalFrameInputs& Out)
{
	static FWorldLookups Lookups;   // game thread only
	Lookups.Refresh(World);

	Out.UtcMicros = FSimClock::Get().NowMicros();
	const bool bPose = SetCameraPose(Out, CamLatDeg, CamLonDeg, CamAltHaeM);
	Out.UpWorld = SanitizeUpWorld(UpWorld);
	Out.bBaseColorAvailable = CamSimThermalPass::bBaseColorAtTonemapper;
	Out.bBaseColorSrgb      = CamSimThermalPass::bBaseColorSrgbEncoded;

	double SunElevDeg = 0.0;
	if (const ACamSimEnvironment* Env = Lookups.Environment.Get())
	{
		// FoldAtmosphere/FoldWeather keep the snapshot finite (air temperature, visibility > 0, cover in [0, 1]).
		const ACamSimEnvironment::FAtmosphericSnapshot S = Env->GetAtmosphericSnapshot();
		Out.AirTempC     = S.AirTempCelsius;
		Out.CloudCover01 = S.CloudCover01;
		Out.VisibilityM  = S.AtmosphericVisibilityM;
		Out.bFogActive   = S.bFogActive;
		SunElevDeg = Env->GetSunElevationDeg();
	}

	if (UDirectionalLightComponent* Sun = Lookups.Sun.Get())
	{
		const FLinearColor Tr = Lookups.Sky.IsValid()
			? Lookups.Sky->GetAtmosphereTransmitanceOnGroundAtPlanetTop(Sun) : FLinearColor::White;
		// FThermalFrameInputs::SunIlluminanceLux is on a surface facing the sun (the builder applies sin elevation
		// itself), so the horizontal-surface helper is evaluated at the zenith; below the horizon it stays 0.
		Out.SunIlluminanceLux = SunElevDeg > 0.0 ? SunIlluminanceLux(Sun->Intensity, Sun->GetLightColor(), Tr, 90.0) : 0.0;
	}

	if (const FOceanSurface* Ocean = Subsystem.GetOceanSurface())
	{
		if (FMath::IsFinite(Ocean->GetWaterTempC())) Out.WaterTempC = Ocean->GetWaterTempC();
		if (bPose)
		{
			SetSea(Out, Ocean->SeaLevelM(Out.CamLatDeg, Out.CamLonDeg), MaxWaveAmplitudeM(Ocean->GetWaves()));
		}
	}

	if (const FCamSimEntityManager* EM = Subsystem.GetEntityManager())
	{
		EM->GetThermalStencilEntities(Out.Entities);
	}
}

bool CamSimThermal::SetLandCover(FThermalFrameInputs& Out, const FLandCoverWindowData* Window, const FVector& EastWorld, const FVector& NorthWorld)
{
	Out.LandCover = FThermalLandCoverInput();
	if (!Window || Window->Id == 0u || Window->NonZeroTexels <= 0) return false;
	auto Finite = [](const FVector& V) { return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z); };
	if (!Finite(EastWorld) || !Finite(NorthWorld)) return false;
	const FVector E = EastWorld.GetSafeNormal();
	const FVector N = NorthWorld.GetSafeNormal();
	if (E.IsZero() || N.IsZero() || FMath::Abs(FVector::DotProduct(E, N)) > 1e-3) return false;
	FThermalLandCoverInput& L = Out.LandCover;
	L.bValid       = true;
	L.WindowId     = Window->Id;
	L.CentreLatDeg = Window->Spec.CentreLatDeg;
	L.CentreLonDeg = Window->Spec.CentreLonDeg;
	L.Texels       = Window->Spec.Texels;
	L.TexelM       = Window->Spec.TexelM;
	L.EastWorld    = E;
	L.NorthWorld   = N;
	return true;
}

void CamSimThermal::LandCoverAxesWorld(const ACesiumGeoreference& Geo, double LatDeg, double LonDeg, FVector& OutEast, FVector& OutNorth)
{
	const FVector Centre = Geo.TransformLongitudeLatitudeHeightPositionToUnreal(FVector(LonDeg, LatDeg, 0.0));   // longitude first
	const FMatrix EsuToUnreal = Geo.ComputeEastSouthUpToUnrealTransformation(Centre);
	OutEast  = EsuToUnreal.TransformVector(FVector(1.0, 0.0, 0.0)).GetSafeNormal();
	OutNorth = -EsuToUnreal.TransformVector(FVector(0.0, 1.0, 0.0)).GetSafeNormal();
}
