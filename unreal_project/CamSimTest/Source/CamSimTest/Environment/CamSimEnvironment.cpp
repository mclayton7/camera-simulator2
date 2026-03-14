// Copyright CamSim Contributors. All Rights Reserved.

#include "Environment/CamSimEnvironment.h"
#include "CamSimTest.h"
#include "Subsystem/CamSimSubsystem.h"
#include "CIGI/CigiReceiver.h"
#include "Camera/CamSimCamera.h"

#include "CesiumSunSky.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Atmosphere/AtmosphericFogComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Components/VolumetricCloudComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"

// -------------------------------------------------------------------------
// Constructor
// -------------------------------------------------------------------------

ACamSimEnvironment::ACamSimEnvironment()
{
	PrimaryActorTick.bCanEverTick = true;

	// Tick after camera so environment state is applied before next capture
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
}

// -------------------------------------------------------------------------
// BeginPlay — find existing environment actors
// -------------------------------------------------------------------------

void ACamSimEnvironment::BeginPlay()
{
	Super::BeginPlay();

	Subsystem = GetGameInstance()->GetSubsystem<UCamSimSubsystem>();
	if (!Subsystem)
	{
		UE_LOG(LogCamSim, Error, TEXT("ACamSimEnvironment: UCamSimSubsystem not found"));
		return;
	}

	// Find existing environment actors placed in the level
	for (TActorIterator<ADirectionalLight> It(GetWorld()); It; ++It)
	{
		SunLight = *It;
		break;
	}
	for (TActorIterator<ASkyLight> It(GetWorld()); It; ++It)
	{
		SkyLight = *It;
		break;
	}
	for (TActorIterator<ASkyAtmosphere> It(GetWorld()); It; ++It)
	{
		SkyAtmosphere = *It;
		break;
	}
	for (TActorIterator<AExponentialHeightFog> It(GetWorld()); It; ++It)
	{
		HeightFog = *It;
		break;
	}

	// VolumetricCloud is optional — not all levels will have one
	for (TActorIterator<AVolumetricCloud> It(GetWorld()); It; ++It)
	{
		CloudActor = *It;
		break;
	}

	// CesiumSunSky drives the directional light when present — preferred path
	for (TActorIterator<ACesiumSunSky> It(GetWorld()); It; ++It)
	{
		CesiumSunSkyActor = *It;
		break;
	}

	// Phase 19 — cache camera reference for ocean plane tracking (avoid per-tick scan)
	for (TActorIterator<ACamSimCamera> It(GetWorld()); It; ++It)
	{
		CamSimCameraActor = *It;
		break;
	}

	// Copy Phase 18 config once at startup
	Phase18Cfg = Subsystem->GetConfig().Phase18;

	// 18L: Populate runtime zone list from YAML-configured positions
	for (const FCamSimConfig::FPhase18Config::FWeatherZoneConfig& ZCfg : Phase18Cfg.WeatherZoneConfigs)
	{
		FWeatherZone Z;
		Z.ZoneID  = ZCfg.ZoneID;
		Z.LatDeg  = ZCfg.LatDeg;
		Z.LonDeg  = ZCfg.LonDeg;
		Z.RadiusM = ZCfg.RadiusM;
		ActiveWeatherZones.Add(Z);
	}

	// Initialise sun from config StartHour
	const FCamSimConfig& Cfg = Subsystem->GetConfig();
	CurrentCelestial.Hour   = static_cast<uint8>(FMath::FloorToInt(Cfg.StartHour));
	CurrentCelestial.Minute = static_cast<uint8>(FMath::FloorToInt(FMath::Fmod(Cfg.StartHour, 1.0f) * 60.0f));

	// Apply initial state
	ApplyCelestial();

	const FString SunName     = SunLight           ? SunLight->GetName()           : TEXT("NONE");
	const FString SkyName     = SkyLight           ? SkyLight->GetName()           : TEXT("NONE");
	const FString AtmosName   = SkyAtmosphere      ? SkyAtmosphere->GetName()      : TEXT("NONE");
	const FString FogName     = HeightFog          ? HeightFog->GetName()          : TEXT("NONE");
	const FString CloudName   = CloudActor         ? CloudActor->GetName()         : TEXT("NONE");
	const FString CesiumName  = CesiumSunSkyActor  ? CesiumSunSkyActor->GetName()  : TEXT("NONE");
	UE_LOG(LogCamSim, Log,
		TEXT("ACamSimEnvironment: CesiumSunSky=%s Sun=%s SkyLight=%s SkyAtmos=%s Fog=%s Cloud=%s  StartHour=%.1f"),
		*CesiumName, *SunName, *SkyName, *AtmosName, *FogName, *CloudName, Cfg.StartHour);

	// Phase 19 — Ocean
	OceanManager.Init(GetWorld(), this, Subsystem, Subsystem->GetConfig().Phase19);
}

// -------------------------------------------------------------------------
// Tick — drain CIGI queues, apply latest state
// -------------------------------------------------------------------------

void ACamSimEnvironment::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!Subsystem) return;

	FCigiReceiver* Receiver = Subsystem->GetCigiReceiver();
	if (!Receiver) return;

	// Drain-and-keep-latest pattern (same as ACamSimCamera)
	FCigiCelestialState CelState;
	bool bGotCelestial = false;
	while (Receiver->DequeueCelestialState(CelState))
	{
		bGotCelestial = true;
	}
	if (bGotCelestial)
	{
		CurrentCelestial = CelState;
		if (!bReceivedCelestial)
		{
			UE_LOG(LogCamSim, Log, TEXT("ACamSimEnvironment: first celestial packet received (time=%02d:%02d)"),
				CelState.Hour, CelState.Minute);
		}
		bReceivedCelestial = true;
		ApplyCelestial();
	}

	FCigiAtmosphereState AtmState;
	bool bGotAtmos = false;
	while (Receiver->DequeueAtmosphereState(AtmState))
	{
		bGotAtmos = true;
	}
	if (bGotAtmos)
	{
		CurrentAtmosphere = AtmState;
		if (!bReceivedAtmosphere)
		{
			UE_LOG(LogCamSim, Log, TEXT("ACamSimEnvironment: first atmosphere packet received (vis=%.0fm)"),
				AtmState.Visibility);
		}
		bReceivedAtmosphere = true;
		ApplyAtmosphere();
	}

	FCigiWeatherState WxState;
	bool bGotWeather = false;
	while (Receiver->DequeueWeatherState(WxState))
	{
		if (WxState.RegionId > 0)
		{
			FWeatherZoneParams ZParams;
			ZParams.FogDensity    = FMath::Lerp(0.0f, 0.1f, WxState.Coverage / 100.0f);
			ZParams.VisibilityM   = WxState.VisibilityRng;
			ZParams.CloudCoverage = WxState.Coverage / 100.0f;
			UpdateWeatherZone(WxState.RegionId, ZParams);
		}
		else
		{
			bGotWeather = true;
		}
	}
	if (bGotWeather)
	{
		CurrentWeather = WxState;
		if (!bReceivedWeather)
		{
			UE_LOG(LogCamSim, Log, TEXT("ACamSimEnvironment: first weather packet received (coverage=%.0f%%  base=%.0fm)"),
				WxState.Coverage, WxState.BaseElev);
		}
		bReceivedWeather = true;

		// 18L: capture fog baseline before weather apply (for zone blending reference)
		if (HeightFog)
		{
			UExponentialHeightFogComponent* FogComp = HeightFog->GetComponent();
			if (FogComp)
			{
				GlobalWeatherBaseline.FogDensity = FogComp->FogDensity;
			}
		}

		ApplyWeather();
	}

	// 18L: Blend zone weather toward camera
	BlendWeatherZones();

	// Phase 19 — drain CIGI Wave Control queue
	{
		FCigiWaveState WaveState;
		while (Receiver->DequeueWaveState(WaveState))
		{
			OceanManager.ApplyWaveState(WaveState);
		}
	}

	// Phase 19 — notify ocean of sky changes once per tick (coalesced from Apply* calls above)
	if (bGotCelestial || bGotAtmos || bGotWeather)
	{
		OnAtmosphereChanged();
	}

	// Phase 19 — tick ocean surface (wave time + plane reposition)
	{
		const FVector CamLoc = CamSimCameraActor
			? CamSimCameraActor->GetActorLocation()
			: FVector::ZeroVector;
		OceanManager.Tick(DeltaTime, CamLoc);
	}
}

// -------------------------------------------------------------------------
// Simplified solar position algorithm
// -------------------------------------------------------------------------

FVector2D ACamSimEnvironment::ComputeSunPosition(
	float Hour, int32 DayOfYear, double Latitude)
{
	// Solar declination (Spencer, 1971 approximation)
	const float B = (360.0f / 365.0f) * (DayOfYear - 81);
	const float BRad = FMath::DegreesToRadians(B);
	const float Declination = 23.45f * FMath::Sin(BRad);

	// Hour angle: 0 at solar noon (12:00), 15°/hour
	const float HourAngle = (Hour - 12.0f) * 15.0f;

	const float LatRad  = FMath::DegreesToRadians(static_cast<float>(Latitude));
	const float DeclRad = FMath::DegreesToRadians(Declination);
	const float HARad   = FMath::DegreesToRadians(HourAngle);

	// Solar elevation angle
	const float SinElev = FMath::Sin(LatRad) * FMath::Sin(DeclRad)
	                     + FMath::Cos(LatRad) * FMath::Cos(DeclRad) * FMath::Cos(HARad);
	const float Elevation = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(SinElev, -1.0f, 1.0f)));

	// Solar azimuth angle (measured from south, converted to compass bearing)
	const float CosAz = (FMath::Sin(DeclRad) - FMath::Sin(LatRad) * SinElev)
	                   / FMath::Max(FMath::Cos(LatRad) * FMath::Cos(FMath::DegreesToRadians(Elevation)), 0.001f);
	float Azimuth = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(CosAz, -1.0f, 1.0f)));

	// Azimuth convention: compass bearing from north
	if (HourAngle > 0.0f)
	{
		Azimuth = 360.0f - Azimuth;
	}
	// Shift from "from south" to "from north"
	Azimuth = FMath::Fmod(Azimuth + 180.0f, 360.0f);

	return FVector2D(Elevation, Azimuth);
}

// -------------------------------------------------------------------------
// ApplyCelestial — sun/moon positioning
// -------------------------------------------------------------------------

void ACamSimEnvironment::ApplyCelestial()
{
	const float HourDecimal = static_cast<float>(CurrentCelestial.Hour)
	                        + static_cast<float>(CurrentCelestial.Minute) / 60.0f;

	// Approximate day-of-year from month/day
	static const int32 DaysBeforeMonth[] = { 0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	const int32 MonthIdx = FMath::Clamp(static_cast<int32>(CurrentCelestial.Month), 1, 12);
	const int32 DayOfYear = DaysBeforeMonth[MonthIdx] + FMath::Clamp(static_cast<int32>(CurrentCelestial.Day), 1, 31);

	// Use camera start latitude for sun position (good enough for visual fidelity)
	const double Latitude = Subsystem ? Subsystem->GetConfig().StartLatitude : 38.0;

	const FVector2D SunPos = ComputeSunPosition(HourDecimal, DayOfYear, Latitude);
	const float SunElevation = SunPos.X;
	const float SunAzimuth   = SunPos.Y;

	UE_LOG(LogCamSim, Log,
		TEXT("ACamSimEnvironment: time=%02d:%02d  sun elev=%.1f az=%.1f"),
		CurrentCelestial.Hour, CurrentCelestial.Minute, SunElevation, SunAzimuth);

	// Drive CesiumSunSky's solar time — it owns the directional light rotation.
	// Fall back to manual rotation only when no CesiumSunSky is in the level.
	if (CesiumSunSkyActor)
	{
		CesiumSunSkyActor->SolarTime = static_cast<double>(HourDecimal);
		if (CurrentCelestial.bDateVld)
		{
			CesiumSunSkyActor->Month = FMath::Clamp(static_cast<int32>(CurrentCelestial.Month), 1, 12);
			CesiumSunSkyActor->Day   = FMath::Clamp(static_cast<int32>(CurrentCelestial.Day),   1, 31);
			const int32 Year = static_cast<int32>(CurrentCelestial.Year);
			if (Year >= 1800 && Year <= 2200)
			{
				CesiumSunSkyActor->Year = Year;
			}
			else
			{
				UE_LOG(LogCamSim, Warning,
					TEXT("ACamSimEnvironment: celestial packet has invalid year %d — skipping date update"), Year);
			}
		}
		CesiumSunSkyActor->UpdateSun();
	}
	else if (SunLight)
	{
		// Fallback: manual rotation when no CesiumSunSky actor exists in the level
		FRotator SunRotation(-SunElevation, SunAzimuth, 0.0f);
		SunLight->SetActorRotation(SunRotation);
	}

	// Intensity/colour tweaks on the directional light (applies in both paths)
	if (SunLight)
	{
		UDirectionalLightComponent* LightComp = Cast<UDirectionalLightComponent>(SunLight->GetLightComponent());
		if (LightComp)
		{
			// Intensity based on elevation
			if (SunElevation > 10.0f)
			{
				// Full daylight
				LightComp->SetIntensity(10.0f);  // lux
				LightComp->SetLightColor(FLinearColor(1.0f, 0.98f, 0.95f));
			}
			else if (SunElevation > 0.0f)
			{
				// Golden hour — warm orange, reduced intensity
				const float T = SunElevation / 10.0f; // 0..1
				LightComp->SetIntensity(FMath::Lerp(3.0f, 10.0f, T));
				FLinearColor Warm = FLinearColor::LerpUsingHSV(
					FLinearColor(1.0f, 0.6f, 0.3f),   // warm orange
					FLinearColor(1.0f, 0.98f, 0.95f),  // neutral white
					T);
				LightComp->SetLightColor(Warm);
			}
			else if (SunElevation > -6.0f)
			{
				// Civil twilight — dim warm light
				const float T = (SunElevation + 6.0f) / 6.0f; // 0..1
				LightComp->SetIntensity(FMath::Lerp(0.1f, 3.0f, T));
				LightComp->SetLightColor(FLinearColor(0.5f, 0.4f, 0.6f)); // dusky purple
			}
			else
			{
				// Night — very dim blue light (moonlight approximation)
				LightComp->SetIntensity(0.02f);
				LightComp->SetLightColor(FLinearColor(0.3f, 0.35f, 0.5f)); // cool blue
			}
		}
	}

	// Recapture sky light when sun elevation changes significantly
	if (SkyLight)
	{
		const float ElevDelta = FMath::Abs(SunElevation - PrevSunElevation);
		if (ElevDelta > 2.0f)
		{
			USkyLightComponent* SkyComp = SkyLight->GetLightComponent();
			if (SkyComp)
			{
				SkyComp->RecaptureSky();
			}
			PrevSunElevation = SunElevation;
		}
	}
}

// -------------------------------------------------------------------------
// ApplyAtmosphere — fog/visibility
// -------------------------------------------------------------------------

void ACamSimEnvironment::ApplyAtmosphere()
{
	if (!HeightFog) return;

	UExponentialHeightFogComponent* FogComp = HeightFog->GetComponent();
	if (!FogComp) return;

	if (!CurrentAtmosphere.bAtmosEn)
	{
		FogComp->SetVisibility(false);
		return;
	}
	FogComp->SetVisibility(true);

	// Beer-Lambert: optical depth 1 at visibility distance
	// FogDensity = 3.912 / Visibility  (ln(50)/visibility for 2% threshold)
	const float ClampedVis = FMath::Clamp(CurrentAtmosphere.Visibility, 10.0f, 200000.0f);
	const float Density = FMath::Clamp(3.912f / ClampedVis, 0.00001f, 0.1f);

	FogComp->SetFogDensity(Density);

	// Inscattering color based on time-of-day
	const float HourDecimal = static_cast<float>(CurrentCelestial.Hour)
	                         + static_cast<float>(CurrentCelestial.Minute) / 60.0f;
	const FVector2D SunPos = ComputeSunPosition(HourDecimal,
		FMath::Clamp(static_cast<int32>(CurrentCelestial.Day), 1, 365),
		Subsystem ? Subsystem->GetConfig().StartLatitude : 38.0);

	if (SunPos.X > 10.0f)
	{
		// Daytime — neutral white/blue fog
		FogComp->SetFogInscatteringColor(FLinearColor(0.65f, 0.72f, 0.78f));
	}
	else if (SunPos.X > 0.0f)
	{
		// Golden hour — warm fog
		FogComp->SetFogInscatteringColor(FLinearColor(0.8f, 0.6f, 0.4f));
	}
	else
	{
		// Night — dark blue-grey fog
		FogComp->SetFogInscatteringColor(FLinearColor(0.1f, 0.12f, 0.18f));
	}

	// Update Phase 18 atmospheric snapshot
	CachedAtmosSnapshot.AtmosphericVisibilityM = CurrentAtmosphere.Visibility;

	ApplySecondFogLayer();
	ApplySkyAtmosphericScattering();

	UE_LOG(LogCamSim, Verbose,
		TEXT("ACamSimEnvironment: visibility=%.0fm  fogDensity=%.6f"),
		CurrentAtmosphere.Visibility, Density);
}

// -------------------------------------------------------------------------
// ApplyWeather — cloud coverage
// -------------------------------------------------------------------------

void ACamSimEnvironment::ApplyWeather()
{
	if (!CurrentWeather.bWeatherEn)
	{
		// Weather disabled — clear skies; reduce fog if we had weather-driven fog
		if (HeightFog && !bReceivedAtmosphere)
		{
			UExponentialHeightFogComponent* FogComp = HeightFog->GetComponent();
			if (FogComp)
			{
				FogComp->SetFogDensity(0.00002f); // Clear day baseline
			}
		}
		return;
	}

	const float Coverage01 = FMath::Clamp(CurrentWeather.Coverage / 100.0f, 0.0f, 1.0f);

	// Fallback: adjust fog to simulate overcast when no volumetric cloud actor exists
	if (HeightFog)
	{
		UExponentialHeightFogComponent* FogComp = HeightFog->GetComponent();
		if (FogComp)
		{
			// Layer fog between weather visibility range and base atmosphere
			const float WeatherVis = FMath::Clamp(CurrentWeather.VisibilityRng, 100.0f, 200000.0f);
			const float AtmosVis   = bReceivedAtmosphere ? CurrentAtmosphere.Visibility : 50000.0f;
			const float EffectiveVis = FMath::Lerp(AtmosVis, WeatherVis, Coverage01);
			const float Density = FMath::Clamp(3.912f / EffectiveVis, 0.00001f, 0.1f);
			FogComp->SetFogDensity(Density);

			// Overcast makes fog more grey
			const FLinearColor OvercastColor(0.55f, 0.58f, 0.62f);
			const FLinearColor CurrentColor = FogComp->FogInscatteringLuminance;
			FogComp->SetFogInscatteringColor(
				FLinearColor::LerpUsingHSV(CurrentColor, OvercastColor, Coverage01 * 0.5f));
		}
	}

	// If SkyLight exists, reduce intensity with heavy overcast
	if (SkyLight)
	{
		USkyLightComponent* SkyComp = SkyLight->GetLightComponent();
		if (SkyComp)
		{
			// Reduce sky contribution under heavy cloud cover
			const float IntScale = FMath::Lerp(1.0f, 0.4f, Coverage01);
			SkyComp->SetIntensity(IntScale);
		}
	}

	// Drive VolumetricCloud layer altitude and thickness from CIGI
	// CIGI BaseElev/Thickness are in metres; UE VolumetricCloud expects km
	if (CloudActor)
	{
		UVolumetricCloudComponent* CloudComp = CloudActor->FindComponentByClass<UVolumetricCloudComponent>();
		if (CloudComp)
		{
			CloudComp->SetLayerBottomAltitude(FMath::Max(CurrentWeather.BaseElev   / 1000.0f, 0.1f));
			CloudComp->SetLayerHeight        (FMath::Max(CurrentWeather.Thickness  / 1000.0f, 0.1f));

			// 18A: Show/hide cloud component based on coverage and enable flag
			if (Phase18Cfg.bVolumetricClouds)
			{
				CloudComp->SetVisibility(Coverage01 > 0.01f);
			}

			// 18B: Drive cloud shadow from coverage threshold
			SetCloudShadowsEnabled(
				Phase18Cfg.bVolumetricClouds && Coverage01 > 0.1f,
				Phase18Cfg.CloudShadowStrength);
		}
	}

	// Update Phase 18 atmospheric snapshot with weather data
	{
		CachedAtmosSnapshot.WeatherSeverity = Coverage01;

		// Derive precipitation type from coverage/visibility heuristic
		// CIGI WeatherState does not carry an explicit precip type, so we infer:
		// heavy coverage + restricted visibility = rain; below-freezing = snow
		const bool bHeavy = (Coverage01 > 0.6f) && (CurrentWeather.VisibilityRng < 5000.0f);
		if (bHeavy)
		{
			// AirTempCelsius < 2°C → snow, else rain
			CachedAtmosSnapshot.WeatherPrecipType = (CachedAtmosSnapshot.AirTempCelsius < 2.0f) ? 2 : 1;
		}
		else
		{
			CachedAtmosSnapshot.WeatherPrecipType = 0;
		}
	}

	ApplyGodRays();

	UE_LOG(LogCamSim, Verbose,
		TEXT("ACamSimEnvironment: weather coverage=%.0f%%  baseElev=%.0fm  thickness=%.0fm"),
		CurrentWeather.Coverage, CurrentWeather.BaseElev, CurrentWeather.Thickness);
}

// -------------------------------------------------------------------------
// Phase 18L: Weather zone blending
// -------------------------------------------------------------------------

void ACamSimEnvironment::SetCameraPosition(double LatDeg, double LonDeg)
{
	CameraLatDeg = LatDeg;
	CameraLonDeg = LonDeg;
}

void ACamSimEnvironment::UpdateWeatherZone(uint16 RegionId, const FWeatherZoneParams& Params)
{
	for (FWeatherZone& Z : ActiveWeatherZones)
	{
		if (Z.ZoneID == static_cast<int32>(RegionId))
		{
			Z.Params = Params;
			return;
		}
	}
	// Zone not in runtime list — it may have a position configured in YAML.
	// Ignore if not pre-configured (position-less zones cannot be blended).
	UE_LOG(LogCamSim, Verbose,
		TEXT("ACamSimEnvironment: RegionId %d has no YAML position config — ignoring zone update."),
		static_cast<int32>(RegionId));
}

float ACamSimEnvironment::GreatCircleApproxM(double Lat1, double Lon1, double Lat2, double Lon2)
{
	// Flat-earth approximation, valid for radii < ~100 km
	const double dLat = FMath::DegreesToRadians(Lat2 - Lat1);
	const double dLon = FMath::DegreesToRadians(Lon2 - Lon1)
	                  * FMath::Cos(FMath::DegreesToRadians((Lat1 + Lat2) * 0.5));
	return static_cast<float>(FMath::Sqrt(dLat*dLat + dLon*dLon) * 6371000.0);
}

void ACamSimEnvironment::BlendWeatherZones()
{
	if (!Phase18Cfg.bWeatherZones || ActiveWeatherZones.IsEmpty()) return;

	float                     BestAlpha  = 0.0f;
	const FWeatherZoneParams* BestParams = nullptr;

	for (const FWeatherZone& Z : ActiveWeatherZones)
	{
		const float DistM = GreatCircleApproxM(CameraLatDeg, CameraLonDeg, Z.LatDeg, Z.LonDeg);
		const float Alpha = FMath::Clamp(1.0f - (DistM / Z.RadiusM), 0.0f, 1.0f);
		if (Alpha > BestAlpha) { BestAlpha = Alpha; BestParams = &Z.Params; }
	}

	if (BestAlpha <= 0.0f || !BestParams) return;

	// Blend fog
	if (HeightFog)
	{
		UExponentialHeightFogComponent* FogComp = HeightFog->GetComponent();
		if (FogComp)
		{
			FogComp->SetFogDensity(
				FMath::Lerp(GlobalWeatherBaseline.FogDensity, BestParams->FogDensity, BestAlpha));
		}
	}
}

// -------------------------------------------------------------------------
// Phase 18 helpers
// -------------------------------------------------------------------------

void ACamSimEnvironment::SetCloudShadowsEnabled(bool bEnabled, float Strength)
{
	if (!SunLight) return;
	UDirectionalLightComponent* LightComp =
		Cast<UDirectionalLightComponent>(SunLight->GetLightComponent());
	if (!LightComp) return;
	LightComp->bCastCloudShadows   = bEnabled;
	LightComp->CloudShadowStrength = FMath::Clamp(Strength, 0.0f, 1.0f);
	LightComp->MarkRenderStateDirty();
}

void ACamSimEnvironment::ApplySecondFogLayer()
{
	if (!HeightFog || !Phase18Cfg.bSecondFog) return;

	UExponentialHeightFogComponent* FogComp = HeightFog->GetComponent();
	if (!FogComp) return;

	FogComp->SecondFogData.FogDensity      = FMath::Clamp(Phase18Cfg.FogDensity,       0.0f, 1.0f);
	FogComp->SecondFogData.FogHeightFalloff = FMath::Clamp(Phase18Cfg.FogHeightFalloff, 0.0f, 1.0f);
	FogComp->MarkRenderStateDirty();
}

void ACamSimEnvironment::ApplyGodRays()
{
	if (!SunLight || !Phase18Cfg.bGodRays) return;

	UDirectionalLightComponent* LightComp = Cast<UDirectionalLightComponent>(SunLight->GetLightComponent());
	if (!LightComp) return;

	LightComp->bEnableLightShaftBloom    = true;
	LightComp->bEnableLightShaftOcclusion = true;
	LightComp->LightShaftOverrideDirection = FVector::ZeroVector; // auto-from-sun

	// Scale bloom tint intensity by config
	const float Intensity = FMath::Clamp(Phase18Cfg.GodRayIntensity, 0.0f, 4.0f);
	LightComp->BloomScale = Intensity;
	LightComp->MarkRenderStateDirty();
}

void ACamSimEnvironment::ApplySkyAtmosphericScattering()
{
	if (!SkyAtmosphere || !Phase18Cfg.bAtmosphericScattering) return;

	USkyAtmosphereComponent* AtmosComp = SkyAtmosphere->GetComponent();
	if (!AtmosComp) return;

	// Base Rayleigh scattering coefficient (UE default ~0.0331 /km at sea level)
	// We multiply by the user's Rayleigh factor
	const float RayleighBase = 0.0331f;
	AtmosComp->RayleighScatteringScale = FMath::Clamp(Phase18Cfg.RayleighScattering * RayleighBase, 0.0f, 1.0f);

	// Base Mie scattering coefficient (UE default ~0.003996 /km)
	const float MieBase = 0.003996f;
	AtmosComp->MieScatteringScale = FMath::Clamp(Phase18Cfg.MieScattering * MieBase, 0.0f, 1.0f);

	AtmosComp->MarkRenderStateDirty();
}

// -------------------------------------------------------------------------
// Phase 19 — Ocean atmosphere bridge
// -------------------------------------------------------------------------

void ACamSimEnvironment::OnAtmosphereChanged()
{
	OceanManager.OnAtmosphereChanged();
}
