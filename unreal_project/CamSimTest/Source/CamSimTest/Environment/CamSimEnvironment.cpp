// Copyright CamSim Contributors. All Rights Reserved.

#include "Environment/CamSimEnvironment.h"
#include "CamSimTest.h"
#include "Subsystem/CamSimSubsystem.h"
#include "CIGI/CigiReceiver.h"
#include "Camera/CamSimCamera.h"
#include "Time/SimClock.h"
#include "Hosts/CigiCommands.h"

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

	// Same group as ACamSimCamera, which lists this actor as a tick
	// prerequisite so environment changes reach the same frame's capture.
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
}

// -------------------------------------------------------------------------
// BeginPlay — find existing environment actors
// -------------------------------------------------------------------------

void ACamSimEnvironment::BeginPlay()
{
	Super::BeginPlay();

	for (TActorIterator<ACamSimCamera> It(GetWorld()); It; ++It)
	{
		It->AddTickPrerequisiteActor(this);
	}

	Subsystem = GetGameInstance()->GetSubsystem<UCamSimSubsystem>();
	if (!Subsystem)
	{
		UE_LOG(LogCamSim, Error, TEXT("ACamSimEnvironment: UCamSimSubsystem not found"));
		return;
	}

	// Find existing environment actors placed in the level
	if (TActorIterator<ADirectionalLight> It(GetWorld()); It)
	{
		SunLight = *It;
	}
	if (TActorIterator<ASkyLight> It(GetWorld()); It)
	{
		SkyLight = *It;
	}
	if (TActorIterator<ASkyAtmosphere> It(GetWorld()); It)
	{
		SkyAtmosphere = *It;
	}
	if (TActorIterator<AExponentialHeightFog> It(GetWorld()); It)
	{
		HeightFog = *It;
	}

	// VolumetricCloud is optional — not all levels will have one
	if (TActorIterator<AVolumetricCloud> It(GetWorld()); It)
	{
		CloudActor = *It;
	}

	// CesiumSunSky drives the directional light when present — preferred path
	if (TActorIterator<ACesiumSunSky> It(GetWorld()); It)
	{
		CesiumSunSkyActor = *It;
	}

	// Phase 19 — cache camera reference for ocean plane tracking (avoid per-tick scan)
	if (TActorIterator<ACamSimCamera> It(GetWorld()); It)
	{
		CamSimCameraActor = *It;
	}

	// Copy Phase 18 config once at startup
	Phase18Cfg = Subsystem->GetConfig().Phase18;

	// Air temperature until CIGI Atmosphere Control sets it (ROADMAP 4A thermal.air_temperature_c).
	if (FMath::IsFinite(Subsystem->GetConfig().Thermal.AirTemperatureC))
	{
		CachedAtmosSnapshot.AirTempCelsius = Subsystem->GetConfig().Thermal.AirTemperatureC;
	}

	// The sim clock is UTC: CesiumSunSky must not apply a zone or DST.
	const FCamSimConfig& Cfg = Subsystem->GetConfig();
	if (CesiumSunSkyActor)
	{
		CesiumSunSkyActor->TimeZone = 0.0;
		CesiumSunSkyActor->UseDaylightSavingTime = false;
	}
	ApplySun(FSimClock::Get().NowUtc());

	const FString SunName     = SunLight           ? SunLight->GetName()           : TEXT("NONE");
	const FString SkyName     = SkyLight           ? SkyLight->GetName()           : TEXT("NONE");
	const FString AtmosName   = SkyAtmosphere      ? SkyAtmosphere->GetName()      : TEXT("NONE");
	const FString FogName     = HeightFog          ? HeightFog->GetName()          : TEXT("NONE");
	const FString CloudName   = CloudActor         ? CloudActor->GetName()         : TEXT("NONE");
	const FString CesiumName  = CesiumSunSkyActor  ? CesiumSunSkyActor->GetName()  : TEXT("NONE");
	UE_LOG(LogCamSim, Log,
		TEXT("ACamSimEnvironment: CesiumSunSky=%s Sun=%s SkyLight=%s SkyAtmos=%s Fog=%s Cloud=%s  sim time %s"),
		*CesiumName, *SunName, *SkyName, *AtmosName, *FogName, *CloudName, *FSimClock::Get().NowUtc().ToIso8601());

	// Ocean (ROADMAP 2.6)
	OceanManager.Init(GetWorld(), this, Subsystem);
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
		ApplyCelestialControl(CelState);
	}

	// The sun follows the sim clock; the sun moves ~0.004 deg/s, so once a
	// sim second is plenty (and catches clock jumps immediately).
	const uint64 NowMicros = FSimClock::Get().NowMicros();
	if (LastSunMicros == 0 || NowMicros < LastSunMicros || NowMicros - LastSunMicros >= 1'000'000)
	{
		ApplySun(FSimClock::FromMicros(NowMicros));
		LastSunMicros = NowMicros;
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

	// Global weather only: regional weather (RegionId > 0) is not supported and is ignored.
	FCigiWeatherState WxState;
	FCigiWeatherState LatestGlobalWx;
	bool bGotWeather = false;
	while (Receiver->DequeueWeatherState(WxState))
	{
		if (WxState.RegionId == 0)
		{
			LatestGlobalWx = WxState;
			bGotWeather = true;
		}
	}
	if (bGotWeather)
	{
		WxState = LatestGlobalWx;
		CurrentWeather = WxState;
		if (!bReceivedWeather)
		{
			UE_LOG(LogCamSim, Log, TEXT("ACamSimEnvironment: first weather packet received (coverage=%.0f%%  base=%.0fm)"),
				WxState.Coverage, WxState.BaseElev);
		}
		bReceivedWeather = true;
		ApplyWeather();
	}

	// Ocean (ROADMAP 2.6) — drain CIGI Wave Control queue
	{
		FCigiWaveState WaveState;
		while (Receiver->DequeueWaveState(WaveState))
		{
			OceanManager.ApplyWave(CamSim::Cigi::ToOceanWaveCommand(WaveState));
		}
	}

	// Phase 26D — drain CIGI Maritime Surface Conditions queue
	{
		FCigiMaritimeSurfaceState MaritimeState;
		while (Receiver->DequeueMaritimeSurface(MaritimeState))
		{
			OceanManager.ApplyMaritimeSurface(CamSim::Cigi::ToMaritimeSurfaceCommand(MaritimeState));
		}
	}

	// Ocean (ROADMAP 2.6) — anchor, mesh rebuilds and MPC around the sensor
	const FCamSimTelemetry Tel = CamSimCameraActor ? CamSimCameraActor->GetCurrentTelemetry() : FCamSimTelemetry();
	OceanManager.Tick(CamSimCameraActor ? &Tel : nullptr);
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

	// Solar azimuth, a compass bearing from north: the formula gives the angle
	// from north in [0, 180]; afternoon (positive hour angle) is west of south.
	const float CosAz = (FMath::Sin(DeclRad) - FMath::Sin(LatRad) * SinElev)
	                   / FMath::Max(FMath::Cos(LatRad) * FMath::Cos(FMath::DegreesToRadians(Elevation)), 0.001f);
	float Azimuth = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(CosAz, -1.0f, 1.0f)));
	if (HourAngle > 0.0f)
	{
		Azimuth = 360.0f - Azimuth;
	}

	return FVector2D(Elevation, Azimuth);
}

// -------------------------------------------------------------------------
// ApplyCelestialControl — CIGI Celestial Sphere Control → sim clock
// -------------------------------------------------------------------------

void ACamSimEnvironment::ApplyCelestialControl(const FCigiCelestialState& Cel)
{
	// Hosts may send the packet every frame: only a change moves the clock,
	// otherwise time would stick at hh:mm:00.
	const bool bChanged = !bReceivedCelestial
		|| Cel.Hour != LastCelestial.Hour || Cel.Minute != LastCelestial.Minute
		|| Cel.Day != LastCelestial.Day || Cel.Month != LastCelestial.Month || Cel.Year != LastCelestial.Year
		|| Cel.bDateVld != LastCelestial.bDateVld || Cel.bEphemerisEn != LastCelestial.bEphemerisEn;
	if (!bChanged) return;
	LastCelestial = Cel;
	bReceivedCelestial = true;

	// ICD 4.1.9: Hour/Minute/Date override the IG's date and time only when
	// Date/Time Valid is set; Ephemeris Model Enable = continuous time of day.
	FSimClock& Clock = FSimClock::Get();
	if (Cel.bDateVld)
	{
		if (FDateTime::Validate(Cel.Year, Cel.Month, Cel.Day, Cel.Hour, Cel.Minute, 0, 0))
		{
			Clock.SetUtc(FDateTime(Cel.Year, Cel.Month, Cel.Day, Cel.Hour, Cel.Minute));
		}
		else
		{
			UE_LOG(LogCamSim, Warning, TEXT("ACamSimEnvironment: celestial date %02d/%02d/%04d %02d:%02d is invalid — ignored"),
				Cel.Month, Cel.Day, Cel.Year, Cel.Hour, Cel.Minute);
		}
	}
	Clock.SetRate(Cel.bEphemerisEn ? Subsystem->GetConfig().SimTimeRate : 0.0);
	UE_LOG(LogCamSim, Log, TEXT("ACamSimEnvironment: Celestial Sphere Control -> sim time %s, %s"),
		*Clock.NowUtc().ToIso8601(), Cel.bEphemerisEn ? TEXT("continuous") : TEXT("static"));

	LastSunMicros = 0;  // re-apply the sun on this tick
}

// -------------------------------------------------------------------------
// ApplySun — sun position and lighting for a UTC time
// -------------------------------------------------------------------------

void ACamSimEnvironment::ApplySun(const FDateTime& Utc)
{
	const double UtcHours = Utc.GetHour() + Utc.GetMinute() / 60.0 + Utc.GetSecond() / 3600.0;

	// The approximate model below wants local solar time: shift by longitude.
	double Latitude  = Subsystem ? Subsystem->GetConfig().StartLatitude  : 38.0;
	double Longitude = Subsystem ? Subsystem->GetConfig().StartLongitude : 0.0;
	if (const ACamSimCamera* Cam = Subsystem ? Subsystem->GetCamera() : nullptr)
	{
		const FCamSimTelemetry T = Cam->GetCurrentTelemetry();
		Latitude  = T.Latitude;
		Longitude = T.Longitude;
	}
	const float LocalSolarHour = static_cast<float>(FMath::Fmod(UtcHours + Longitude / 15.0 + 48.0, 24.0));
	const int32 DayOfYear = Utc.GetDayOfYear();

	const FVector2D SunPos = ComputeSunPosition(LocalSolarHour, DayOfYear, Latitude);
	const float SunElevation = SunPos.X;
	const float SunAzimuth   = SunPos.Y;
	SunElevationDeg = SunElevation;

	UE_LOG(LogCamSim, Verbose, TEXT("ACamSimEnvironment: sim time %s  sun elev=%.1f az=%.1f"),
		*Utc.ToIso8601(), SunElevation, SunAzimuth);

	// Drive CesiumSunSky's solar time — it owns the directional light rotation.
	// Fall back to manual rotation only when no CesiumSunSky is in the level.
	if (CesiumSunSkyActor)
	{
		// TimeZone is 0 and DST off (BeginPlay), so SolarTime is UTC.
		CesiumSunSkyActor->SolarTime = FMath::Min(UtcHours, 23.9999);
		CesiumSunSkyActor->Day   = Utc.GetDay();
		CesiumSunSkyActor->Month = Utc.GetMonth();
		CesiumSunSkyActor->Year  = Utc.GetYear();
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

void ACamSimEnvironment::FoldAtmosphere(FAtmosphericSnapshot& S, const FCigiAtmosphereState& A)
{
	if (FMath::IsFinite(A.AirTemp)) S.AirTempCelsius = A.AirTemp;
	if (FMath::IsFinite(A.Visibility) && A.Visibility > 0.0f) S.AtmosphericVisibilityM = A.Visibility;
	if (FMath::IsFinite(A.Humidity)) S.RelativeHumidity = FMath::Clamp(A.Humidity / 100.0f, 0.0f, 1.0f);
	S.bFogActive = S.AtmosphericVisibilityM < FogVisibilityM;
}

void ACamSimEnvironment::FoldWeather(FAtmosphericSnapshot& S, const FCigiWeatherState& W)
{
	S.CloudCover01 = FMath::IsFinite(W.Coverage) ? FMath::Clamp(W.Coverage / 100.0f, 0.0f, 1.0f) : 0.0f;
}

void ACamSimEnvironment::ApplyAtmosphere()
{
	// Snapshot first: the CIGI values hold whether or not there is a fog actor to drive (ROADMAP 4A).
	FoldAtmosphere(CachedAtmosSnapshot, CurrentAtmosphere);
	if (!CurrentAtmosphere.bAtmosEn) CachedAtmosSnapshot.bFogActive = false;   // the fog is not drawn

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
	const FVector2D SunPos(SunElevationDeg, 0.0f);

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

	ApplySecondFogLayer();

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
		CachedAtmosSnapshot.CloudCover01 = 0.0f;   // no weather: clear sky (ROADMAP 4A)
		return;
	}

	FoldWeather(CachedAtmosSnapshot, CurrentWeather);
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

	UE_LOG(LogCamSim, Verbose,
		TEXT("ACamSimEnvironment: weather coverage=%.0f%%  baseElev=%.0fm  thickness=%.0fm"),
		CurrentWeather.Coverage, CurrentWeather.BaseElev, CurrentWeather.Thickness);
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
