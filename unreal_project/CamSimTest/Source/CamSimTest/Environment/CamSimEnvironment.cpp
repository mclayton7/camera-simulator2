// Copyright CamSim Contributors. All Rights Reserved.

#include "Environment/CamSimEnvironment.h"
#include "CamSimTest.h"
#include "Subsystem/CamSimSubsystem.h"
#include "CIGI/CigiReceiver.h"
#include "Camera/CamSimCamera.h"
#include "Time/SimClock.h"
#include "Hosts/CigiCommands.h"

#include "CesiumGeoreference.h"
#include "CesiumSunSky.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Atmosphere/AtmosphericFogComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Components/VolumetricCloudComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Engine/GameInstance.h"
#include "Environment/StarField.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

namespace
{
	TAutoConsoleVariable<int32> CVarSkyLog(
		TEXT("camsim.Sky.Log"),
		0,
		TEXT("ROADMAP 6A: 1 logs one LogCamSim line per sky apply (once per sim second and on clock/enable changes): ")
		TEXT("'6A sky: utc=... sun_el=... sun_ground_lux=... moon_el=... moon_phase=... moon_illum=... moon_ground_lux=... ")
		TEXT("floor_cdm2=... band=day|night stars=0|1 moon_az=... moon_norm=recomputes/total ms' (scripts/night_check.py parses it; moon az/el to 1e-5 deg for N3)."),
		ECVF_Default);

	void SetCachedLightingPreExposure(float Ev)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("r.EyeAdaptation.CachedLightingPreExposure")))
		{
			Var->Set(Ev, ECVF_SetByCode);
		}
	}
}

// -------------------------------------------------------------------------
// Constructor
// -------------------------------------------------------------------------

ACamSimEnvironment::ACamSimEnvironment()
{
	PrimaryActorTick.bCanEverTick = true;

	// Same group as ACamSimCamera, which lists this actor as a tick
	// prerequisite so environment changes reach the same frame's capture.
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;

	// Root for the moon light and the sky dome (ROADMAP 6A).
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
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

	// Find existing environment actors placed in the level. The sun, sky light and atmosphere are CesiumSunSky's
	// components (SetUpSky); Main.umap has no fog actor (the fog path is dead there, ROADMAP 8.6).
	if (TActorIterator<AExponentialHeightFog> It(GetWorld()); It)
	{
		HeightFog = *It;
	}

	// VolumetricCloud is optional — not all levels will have one
	if (TActorIterator<AVolumetricCloud> It(GetWorld()); It)
	{
		CloudActor = *It;
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

	// ROADMAP 6A: sun, moon, dome; the cached-lighting band starts in the day band so it and the cvar agree.
	CelestialCfg = Subsystem->GetConfig().Celestial;
	SetUpSky();
	SetCachedLightingPreExposure(Band.CachedEv(CelestialCfg.CachedExposure));
	ApplySky(FSimClock::Get().NowUtc());
	ApplySkyFrame();

	const FString FogName     = HeightFog          ? HeightFog->GetName()          : TEXT("NONE");
	const FString CloudName   = CloudActor         ? CloudActor->GetName()         : TEXT("NONE");
	const FString CesiumName  = CesiumSunSkyActor  ? CesiumSunSkyActor->GetName()  : TEXT("NONE");
	const FString SunName     = SunLightComp       ? SunLightComp->GetOwner()->GetName() : TEXT("NONE");
	UE_LOG(LogCamSim, Log,
		TEXT("ACamSimEnvironment: CesiumSunSky=%s Sun=%s Fog=%s Cloud=%s  sim time %s"),
		*CesiumName, *SunName, *FogName, *CloudName, *FSimClock::Get().NowUtc().ToIso8601());

	// Ocean (ROADMAP 2.6)
	OceanManager.Init(GetWorld(), this, Subsystem);
}

void ACamSimEnvironment::EndPlay(const EEndPlayReason::Type Reason)
{
	SkyRecaptureHold.Release();   // never leave the sky capture un-time-sliced after a switch mid-hold
	Super::EndPlay(Reason);
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
		ApplyCelestialControl(CelState, FSimClock::Get());
	}

	// The sun and moon follow the sim clock; they move ~0.004-0.01 deg/s, so once a
	// sim second is plenty (and catches clock jumps immediately). The ephemeris is per observer, so a camera move
	// re-applies it too (the clock may stand still: lockstep, a static CIGI time).
	const uint64 NowMicros = FSimClock::Get().NowMicros();
	double ObsLat = 0.0, ObsLon = 0.0, ObsAltM = 0.0;
	GetObserver(ObsLat, ObsLon, ObsAltM);
	if (ShouldReapplySky(NowMicros, LastSunMicros, ObsLat, ObsLon, LastSkyLat, LastSkyLon))
	{
		ApplySky(FSimClock::FromMicros(NowMicros));
		LastSunMicros = NowMicros;
		LastSkyLat = ObsLat;
		LastSkyLon = ObsLon;
	}
	// Every tick: directions in the current UE frame, the dome at the camera, the cached-lighting band (ROADMAP 6A).
	ApplySkyFrame();
	UpdateCachedExposureBand();

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

void ACamSimEnvironment::ApplyCelestialControl(const FCigiCelestialState& Cel, FSimClock& Clock)
{
	// Hosts may send the packet every frame: only a change moves the clock,
	// otherwise time would stick at hh:mm:00. An enable / star intensity change
	// alone only re-applies the sky (ROADMAP 6A): it must not rewind the clock.
	const FCigiCelestialState* Prev = bReceivedCelestial ? &LastCelestial : nullptr;
	if (!IsCelestialChange(Prev, Cel)) return;
	const bool bClockChange = IsCelestialClockChange(Prev, Cel);
	LastCelestial = Cel;
	bReceivedCelestial = true;
	LastSunMicros = 0;  // re-apply the sky (date/time and enables) on this tick
	if (!bClockChange)
	{
		UE_LOG(LogCamSim, Log, TEXT("ACamSimEnvironment: Celestial Sphere Control -> sun %d moon %d stars %d (intensity %.2f)"),
			Cel.bSunEn ? 1 : 0, Cel.bMoonEn ? 1 : 0, Cel.bStarEn ? 1 : 0, Cel.StarInt);
		return;
	}

	// ICD 4.1.9: Hour/Minute/Date override the IG's date and time only when
	// Date/Time Valid is set; Ephemeris Model Enable = continuous time of day.
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
	Clock.SetRate(Cel.bEphemerisEn ? (Subsystem ? Subsystem->GetConfig().SimTimeRate : 1.0) : 0.0);
	UE_LOG(LogCamSim, Log, TEXT("ACamSimEnvironment: Celestial Sphere Control -> sim time %s, %s"),
		*Clock.NowUtc().ToIso8601(), Cel.bEphemerisEn ? TEXT("continuous") : TEXT("static"));
}

bool ACamSimEnvironment::IsCelestialClockChange(const FCigiCelestialState* Prev, const FCigiCelestialState& Next)
{
	if (!Prev) return true;
	const FCigiCelestialState& P = *Prev;
	return Next.Hour != P.Hour || Next.Minute != P.Minute
		|| Next.Day != P.Day || Next.Month != P.Month || Next.Year != P.Year
		|| Next.bDateVld != P.bDateVld || Next.bEphemerisEn != P.bEphemerisEn;
}

bool ACamSimEnvironment::ShouldReapplySky(uint64 NowMicros, uint64 LastMicros, double Lat, double Lon, double LastLat, double LastLon)
{
	if (LastMicros == 0 || NowMicros < LastMicros || NowMicros - LastMicros >= 1'000'000) return true;
	if (!FMath::IsFinite(Lat) || !FMath::IsFinite(Lon)) return false;
	// Equirectangular distance on a 6371 km sphere: plenty for a 1 km threshold.
	const double DLat = FMath::DegreesToRadians(Lat - LastLat);
	const double DLon = FMath::DegreesToRadians(FMath::Fmod(Lon - LastLon + 540.0, 360.0) - 180.0)
		* FMath::Cos(FMath::DegreesToRadians(0.5 * (Lat + LastLat)));
	return 6371000.0 * FMath::Sqrt(DLat * DLat + DLon * DLon) > SkyObserverMoveM;
}

bool ACamSimEnvironment::IsCelestialChange(const FCigiCelestialState* Prev, const FCigiCelestialState& Next)
{
	if (!Prev) return true;
	const FCigiCelestialState& P = *Prev;
	const bool bSameStarInt = (P.StarInt == Next.StarInt) || (FMath::IsNaN(P.StarInt) && FMath::IsNaN(Next.StarInt));
	return IsCelestialClockChange(Prev, Next)
		|| Next.bSunEn != P.bSunEn || Next.bMoonEn != P.bMoonEn || Next.bStarEn != P.bStarEn
		|| !bSameStarInt;
}

// -------------------------------------------------------------------------
// ROADMAP 6A — sun, moon, sky dome, cached-lighting band
// -------------------------------------------------------------------------

void ACamSimEnvironment::SetUpSky()
{
	if (bSkySetUp) return;
	bSkySetUp = true;
	UWorld* World = GetWorld();
	if (!World) return;

	if (!CesiumSunSkyActor)
	{
		if (TActorIterator<ACesiumSunSky> It(World); It)
		{
			CesiumSunSkyActor = *It;
		}
	}
	if (CesiumSunSkyActor)
	{
		// The sim clock is UTC: CesiumSunSky must not apply a zone or DST.
		CesiumSunSkyActor->TimeZone = 0.0;
		CesiumSunSkyActor->UseDaylightSavingTime = false;
		SunLightComp = CesiumSunSkyActor->DirectionalLight;
		// Spec finding 5: CesiumSunSky sets 90 (ground transmittance as if the light were at the zenith), so the sun never
		// dimmed or reddened at sunset. −90 (the engine default): sun and moon follow their air mass and the planet.
		if (USkyAtmosphereComponent* Atmos = CesiumSunSkyActor->SkyAtmosphere)
		{
			Atmos->SetTransmittanceMinLightElevationAngle(-90.0f);
			// Ozone as a linear-sRGB band integral, not UE's 680/550/440 nm samples: blue twilight, not magenta.
			const FLinearColor Ozone = FNightIllumination::OzoneAbsorptionPerKm();
			Atmos->SetOtherAbsorptionScale(Ozone.G);
			Atmos->SetOtherAbsorption(FLinearColor(Ozone.R / Ozone.G, 1.0f, Ozone.B / Ozone.G));
		}
		// The sky light's lower hemisphere black (UE's default; Cesium sets false): the real-time capture's lower half is
		// SkyAtmosphere's planet surface lit by the reddened sun, which reached the ground through Lumen's distant
		// lighting and the sky light's SH, and turned the twilight ground view-dependently red (nadir 100 m, sun -6:
		// illuminant R/G x1.35 vs x1.05). A horizontal surface sees no lower hemisphere; terrain bounce is Lumen's.
		if (USkyLightComponent* SkyL = CesiumSunSkyActor->SkyLight)
		{
			SkyL->bLowerHemisphereIsBlack = true;
			SkyL->SetLowerHemisphereColor(FLinearColor::Black);
			SkyL->MarkRenderStateDirty();
		}
	}
	else
	{
		// No CesiumSunSky (test levels): a level directional light, else a spawned one, as atmosphere light 0.
		if (TActorIterator<ADirectionalLight> It(World); It)
		{
			FallbackSun = *It;
		}
		else
		{
			FActorSpawnParameters Params;
			Params.Owner = this;
			FallbackSun = World->SpawnActor<ADirectionalLight>(Params);
		}
		if (FallbackSun)
		{
			SunLightComp = Cast<UDirectionalLightComponent>(FallbackSun->GetLightComponent());
		}
		if (SunLightComp)
		{
			SunLightComp->SetMobility(EComponentMobility::Movable);
			SunLightComp->SetAtmosphereSunLight(true);
			SunLightComp->SetAtmosphereSunLightIndex(0);
		}
	}
	if (!Georeference)
	{
		Georeference = ACesiumGeoreference::GetDefaultGeoreference(World);
	}

	// Moon: atmosphere light 1 (spec finding 3: bAtmosphereSunLight defaults to true, so the index must be set). The dome
	// draws the moon disc with its phase, so the engine's uniform index-1 disc is zeroed.
	if (!MoonLight)
	{
		MoonLight = NewObject<UDirectionalLightComponent>(this, TEXT("CamSimMoonLight"));
		MoonLight->SetupAttachment(GetRootComponent());
		MoonLight->SetMobility(EComponentMobility::Movable);
		MoonLight->SetAtmosphereSunLight(true);
		MoonLight->SetAtmosphereSunLightIndex(1);
		MoonLight->ForwardShadingPriority = -1;
		MoonLight->bCastShadowsOnClouds = false;   // unsupported at index 1
		MoonLight->CloudScatteredLuminanceScale = FLinearColor::Black;   // ApplySky: white only at night (MoonCloudScatterScale)
		MoonLight->LightSourceAngle = 0.52f;
		MoonLight->AtmosphereSunDiskColorScale = FLinearColor::Black;
		MoonLight->SetIntensity(0.0f);
		MoonLight->SetCastShadows(false);
		MoonLight->SetVisibility(false);
		MoonLight->RegisterComponent();
		AddInstanceComponent(MoonLight);
	}

	SkyDome.Init(this, CelestialCfg);

	const FString Catalogue = CelestialCfg.StarCatalog.IsEmpty() ? FStarField::DefaultPath() : CelestialCfg.StarCatalog;
	UE_LOG(LogCamSim, Log,
		TEXT("6A sky: sun=%s moon light=atmosphere index 1, dome=%s, stars=%d texels (%s), catalogue %s, georeference=%s"),
		SunLightComp ? *SunLightComp->GetReadableName() : TEXT("NONE"),
		SkyDome.IsInitialized() ? TEXT("on") : TEXT("OFF"),
		SkyDome.GetStarTexelCount(), SkyDome.HasStars() ? TEXT("loaded") : TEXT("none"), *Catalogue,
		Georeference ? *Georeference->GetName() : TEXT("NONE"));
}

void ACamSimEnvironment::GetObserver(double& OutLat, double& OutLon, double& OutAltM) const
{
	const ACamSimCamera* Cam = CamSimCameraActor ? CamSimCameraActor.Get() : (Subsystem ? Subsystem->GetCamera() : nullptr);
	if (Cam)
	{
		const FCamSimTelemetry& T = Cam->GetTelemetry();
		OutLat = T.Latitude;
		OutLon = T.Longitude;
		OutAltM = T.Altitude;
		return;
	}
	if (Subsystem)
	{
		OutLat = Subsystem->GetConfig().StartLatitude;
		OutLon = Subsystem->GetConfig().StartLongitude;
		OutAltM = 0.0;
		return;
	}
	OutLat = Georeference ? Georeference->GetOriginLatitude() : 38.0;
	OutLon = Georeference ? Georeference->GetOriginLongitude() : 0.0;
	OutAltM = Georeference ? Georeference->GetOriginHeight() : 0.0;
}

void ACamSimEnvironment::ApplySky(const FDateTime& Utc)
{
	const double UtcHours = Utc.GetHour() + Utc.GetMinute() / 60.0 + Utc.GetSecond() / 3600.0;

	double Lat = 0.0, Lon = 0.0, AltM = 0.0;
	GetObserver(Lat, Lon, AltM);
	LastEphem = FEphemeris::Compute(Utc, Lat, Lon, AltM);
	LastSkyCenturiesJ2000 = FSkyDomeDriver::CenturiesSinceJ2000(Utc);
	SunElevationDeg = static_cast<float>(LastEphem.SunElDeg);

	UE_LOG(LogCamSim, Verbose, TEXT("ACamSimEnvironment: sim time %s  sun elev=%.1f az=%.1f  moon elev=%.1f az=%.1f"),
		*Utc.ToIso8601(), LastEphem.SunElDeg, LastEphem.SunAzDeg, LastEphem.MoonElDeg, LastEphem.MoonAzDeg);

	// CesiumSunSky owns the sun light's rotation (its own solar position at the georeference origin).
	if (CesiumSunSkyActor)
	{
		// TimeZone is 0 and DST off (SetUpSky), so SolarTime is UTC.
		CesiumSunSkyActor->SolarTime = FMath::Min(UtcHours, 23.9999);
		CesiumSunSkyActor->Day   = Utc.GetDay();
		CesiumSunSkyActor->Month = Utc.GetMonth();
		CesiumSunSkyActor->Year  = Utc.GetYear();
		CesiumSunSkyActor->UpdateSun();
	}

	LastEnables = FCelestialEnables::From(CelestialCfg, bReceivedCelestial ? &LastCelestial : nullptr);
	FNightIlluminationInput In;
	In.Ephem = LastEphem;
	In.CloudCover01 = CachedAtmosSnapshot.CloudCover01;
	In.NightFloorLux = CelestialCfg.NightFloorLux;
	In.bSunEnabled = LastEnables.bSun;
	In.bMoonEnabled = LastEnables.bMoon;
	LastIllum = FNightIllumination::Compute(In);

	// k = 1: light intensities are top-of-atmosphere lux; the atmosphere applies transmittance and colour.
	if (SunLightComp)
	{
		SunLightComp->SetIntensity(static_cast<float>(LastIllum.SunToaLux));
		SunLightComp->SetLightColor(FLinearColor::White);
	}
	if (MoonLight)
	{
		const bool bMoonUp = LastIllum.MoonToaLux > 0.0 && LastEphem.MoonElDeg > -1.0;
		MoonLight->SetIntensity(bMoonUp ? static_cast<float>(LastIllum.MoonLightLux) : 0.0f);
		MoonLight->SetLightColor(LastIllum.MoonColor);
		MoonLight->SetVisibility(bMoonUp);
		// The moon on the volumetric clouds is UE's expensive second cloud light (~0.4 ms on the night bench): only while
		// the sun is down (a no-op unless it changes: CloudScatteredLuminanceScale is a render-state property).
		const FLinearColor CloudScale = FNightIllumination::MoonCloudScatterScale(LastEphem.SunElDeg, bMoonUp);
		if (!MoonLight->CloudScatteredLuminanceScale.Equals(CloudScale))
		{
			MoonLight->CloudScatteredLuminanceScale = CloudScale;
			MoonLight->MarkRenderStateDirty();
		}
		// Shadows (VSM) only while the moon is the brighter light: no shadow cost by day.
		MoonLight->SetCastShadows(bMoonUp && LastIllum.MoonGroundLux > LastIllum.SunGroundLux);
	}
	bSkyApplied = true;

	if (CVarSkyLog.GetValueOnGameThread() == 1)
	{
		UE_LOG(LogCamSim, Log,
			TEXT("6A sky: utc=%s sun_el=%.3f sun_ground_lux=%.4g moon_el=%.5f moon_phase=%.2f moon_illum=%.3f moon_ground_lux=%.4g floor_cdm2=%.4g band=%s stars=%d moon_az=%.5f moon_norm=%d/%.1fms"),
			*Utc.ToIso8601(), LastEphem.SunElDeg, LastIllum.SunGroundLux, LastEphem.MoonElDeg, LastEphem.MoonPhaseAngleDeg,
			LastEphem.MoonIlluminatedFraction, LastIllum.MoonGroundLux, LastIllum.FloorRadianceCdM2,
			Band.IsNight() ? TEXT("night") : TEXT("day"), (LastEnables.bStars && SkyDome.HasStars()) ? 1 : 0, LastEphem.MoonAzDeg,
			SkyDome.GetMoonNormaliser().GetRecomputeCount(), SkyDome.GetMoonNormaliser().GetTotalComputeMs());
	}
}

void ACamSimEnvironment::ApplySkyFrame()
{
	if (!bSkyApplied || !Georeference) return;
	double Lat = 0.0, Lon = 0.0, AltM = 0.0;
	GetObserver(Lat, Lon, AltM);
	if (!FMath::IsFinite(Lat) || !FMath::IsFinite(Lon) || !FMath::IsFinite(AltM)) return;
	const FVector CameraWorld = Georeference->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(Lon, Lat, AltM));   // longitude first
	const FMatrix EsuToUnreal = Georeference->ComputeEastSouthUpToUnrealTransformation(CameraWorld);

	// Directional lights point from the source: forward = -direction to it (a rotation matrix, never FQuat::Rotator()).
	if (MoonLight)
	{
		const FVector MoonDir = FSkyDomeDriver::EnuToWorldDir(EsuToUnreal, LastEphem.MoonEnu);
		if (!MoonDir.IsNearlyZero())
		{
			MoonLight->SetWorldRotation(FRotationMatrix::MakeFromX(-MoonDir).ToQuat());
		}
	}
	if (!CesiumSunSkyActor && SunLightComp)
	{
		const FVector SunDir = FSkyDomeDriver::EnuToWorldDir(EsuToUnreal, LastEphem.SunEnu);
		if (!SunDir.IsNearlyZero())
		{
			SunLightComp->SetWorldRotation(FRotationMatrix::MakeFromX(-SunDir).ToQuat());
		}
	}
	SkyDome.Update(LastEphem, LastIllum, LastEnables, EsuToUnreal, CameraWorld, Lat, LastSkyCenturiesJ2000);
}

void ACamSimEnvironment::UpdateCachedExposureBand()
{
	const ACamSimCamera* Cam = CamSimCameraActor.Get();
	if (!Cam) return;
	const FCachedExposureBandConfig& BandCfg = CelestialCfg.CachedExposure;
	// Only EO medians of real frames: in IR the histogram is radiance, after IR -> EO the controller's last median is still
	// the IR one until an EO frame comes back (it flipped the band to night and back in daylight), and while the terrain
	// gate holds frames the first histograms are of an unloaded (black) scene (the smoke run flipped in the first 8 frames).
	float Median = 0.0f;
	const bool bEoMedian = Cam->GetEoSceneMedianLog2(Median);
	const bool bSwitched = Band.Update(Median, bEoMedian && Cam->IsSensorInEo() && Cam->IsTerrainReady(), BandCfg);
	if (bSwitched)
	{
		SetCachedLightingPreExposure(Band.CachedEv(BandCfg));
		UE_LOG(LogCamSim, Log, TEXT("6A: cached lighting band → %s (%.0f EV), scene median 2^%.1f"),
			Band.IsNight() ? TEXT("night") : TEXT("day"), Band.CachedEv(BandCfg), Median);
	}
	// Re-encode the whole sky capture at the new pre-exposure at once (FSkyRecaptureHold): time-sliced, the old cubemap
	// dimmed Lumen's sky lighting for ~1.5 s after a switch.
	SkyRecaptureHold.Tick(bSwitched);
}

#if WITH_DEV_AUTOMATION_TESTS
void ACamSimEnvironment::ApplySkyForTest(const FDateTime& Utc)
{
	SetUpSky();
	ApplySky(Utc);
	ApplySkyFrame();
}
#endif

// -------------------------------------------------------------------------
// Canonical commands (control API, ROADMAP 2.4)
// -------------------------------------------------------------------------

FCigiAtmosphereState ACamSimEnvironment::ToAtmosphereState(const FAtmosphereCommand& A)
{
	FCigiAtmosphereState S;
	S.bAtmosEn    = A.bEnabled;
	S.Humidity    = A.HumidityPct;
	S.AirTemp     = A.AirTempC;
	S.Visibility  = A.VisibilityM;
	S.HorizWindSp = A.HorizWindMps;
	S.VertWindSp  = A.VertWindMps;
	S.WindDir     = A.WindDirDeg;
	S.BaroPress   = A.BaroPressureMb;
	return S;
}

FCigiWeatherState ACamSimEnvironment::ToWeatherState(const FWeatherCommand& W)
{
	FCigiWeatherState S;
	S.RegionId      = W.RegionId;
	S.LayerId       = W.LayerId;
	S.Severity      = W.Severity;
	S.bWeatherEn    = W.bEnabled;
	S.CloudType     = W.CloudType;
	S.Scope         = static_cast<uint8>(W.Scope);   // Global 0, Regional 1, Entity 2 (CIGI's order)
	S.Coverage      = W.CoveragePct;
	S.BaseElev      = W.BaseElevM;
	S.Thickness     = W.ThicknessM;
	S.Transition    = W.TransitionM;
	S.VisibilityRng = W.VisibilityM;
	S.HorizWindSp   = W.HorizWindMps;
	S.VertWindSp    = W.VertWindMps;
	S.WindDir       = W.WindDirDeg;
	return S;
}

void ACamSimEnvironment::ApplyAtmosphereCommand(const FAtmosphereCommand& A)
{
	CurrentAtmosphere = ToAtmosphereState(A);
	bReceivedAtmosphere = true;
	ApplyAtmosphere();
}

void ACamSimEnvironment::ApplyWeatherCommand(const FWeatherCommand& W)
{
	CurrentWeather = ToWeatherState(W);
	bReceivedWeather = true;
	ApplyWeather();
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
	if (FMath::IsFinite(A.WindDir))     S.WindDirectionDeg = A.WindDir;
	if (FMath::IsFinite(A.HorizWindSp)) S.WindSpeedMps     = FMath::Max(0.0f, A.HorizWindSp);
	if (FMath::IsFinite(A.BaroPress) && A.BaroPress > 0.0f) S.BaroPressureMb = A.BaroPress;
	S.bHostAtmosphere = true;
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
		if (CachedAtmosSnapshot.CloudCover01 != 0.0f) LastSunMicros = 0;   // re-apply the sky's cloud factor (ROADMAP 6A)
		CachedAtmosSnapshot.CloudCover01 = 0.0f;   // no weather: clear sky (ROADMAP 4A)
		return;
	}

	const float PrevCloud = CachedAtmosSnapshot.CloudCover01;
	FoldWeather(CachedAtmosSnapshot, CurrentWeather);
	// A new cloud cover re-applies the moon and night floor on this tick (ROADMAP 6A), also with the sim clock frozen.
	if (CachedAtmosSnapshot.CloudCover01 != PrevCloud) LastSunMicros = 0;
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

	// Overcast dims CesiumSunSky's sky light (real-time capture)
	if (CesiumSunSkyActor)
	{
		USkyLightComponent* SkyComp = CesiumSunSkyActor->SkyLight;
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
	UDirectionalLightComponent* LightComp = SunLightComp;
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
