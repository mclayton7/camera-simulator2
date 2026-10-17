// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CIGI/CigiPacketTypes.h"
#include "Config/CamSimConfig.h"
#include "Ocean/FOceanManager.h"
#include "Sim/Commands.h"
#include "Environment/CachedExposureBand.h"
#include "Environment/CelestialEnables.h"
#include "Environment/Ephemeris.h"
#include "Environment/NightIllumination.h"
#include "Environment/SkyDomeDriver.h"
#include "CamSimEnvironment.generated.h"

class ADirectionalLight;
class AExponentialHeightFog;
class ACesiumGeoreference;
class UDirectionalLightComponent;
class AVolumetricCloud;
class ACesiumSunSky;
class UCamSimSubsystem;
class ACamSimCamera;
class FSimClock;

/**
 * ACamSimEnvironment
 *
 * Drives UE5 environment actors (sun, sky, fog, clouds) from CIGI
 * Celestial Sphere Control, Atmosphere Control, and Weather Control packets.
 *
 * ROADMAP 6A (docs/superpowers/specs/2026-10-16-night-sky-6a-design.md): scene scale k = 1. The sun is CesiumSunSky's
 * DirectionalLight at its top-of-atmosphere illuminance (atmosphere light 0); the moon is this actor's own directional
 * light at atmosphere light index 1; the IsSky dome (FSkyDomeDriver) draws the atmosphere, discs, stars and night floor;
 * the Lumen cached-lighting exposure follows FCachedExposureBand.
 *
 * Place one instance in the level or let ACamSimGameMode spawn it.
 */
UCLASS()
class CAMSIMTEST_API ACamSimEnvironment : public AActor
{
	GENERATED_BODY()

public:
	ACamSimEnvironment();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaTime) override;

	/** Current sun elevation in degrees above horizon (Phase 16K; ROADMAP 6A: the ephemeris sun, geometric). */
	float GetSunElevationDeg() const { return SunElevationDeg; }

	/**
	 * Simplified solar position for local solar time Hour: (elevation above the
	 * horizon, compass azimuth from north), degrees.
	 */
	static FVector2D ComputeSunPosition(float Hour, int32 DayOfYear, double Latitude);

	/** Atmospheric state snapshot for the task thread (Phase 18). */
	struct FAtmosphericSnapshot
	{
		float AtmosphericVisibilityM = 10000.0f;
		float RelativeHumidity       = 0.5f;
		float AirTempCelsius         = 15.0f;
		float WeatherSeverity        = 0.0f;
		uint8 WeatherPrecipType      = 0;      // 0=none, 1=rain, 2=snow
		float CloudCover01           = 0.0f;   // CIGI Weather Control coverage / 100, clamped (0 with no weather) (ROADMAP 4A)
		bool  bFogActive             = false;  // atmosphere visibility < FogVisibilityM (ROADMAP 4A)
		// Set once the host has sent Atmosphere Control; the KLV weather tags (35-37, 39, 55) need it.
		bool  bHostAtmosphere        = false;
		float WindDirectionDeg       = 0.0f;     // from, true north
		float WindSpeedMps           = 0.0f;     // horizontal
		float BaroPressureMb         = 1013.25f; // sea level
	};

	/** Visibility below which the atmosphere counts as fog (thermal path extinction, ROADMAP 4A). */
	static constexpr float FogVisibilityM = 10000.0f;

	/**
	 * CIGI Atmosphere Control → snapshot: air temperature (finite only), visibility (finite, > 0), humidity, fog,
	 * wind and pressure (finite only), and marks the host atmosphere as received.
	 */
	static void FoldAtmosphere(FAtmosphericSnapshot& S, const FCigiAtmosphereState& A);
	/** CIGI Weather Control (global) → snapshot cloud cover (coverage % / 100, clamped; non-finite → 0). */
	static void FoldWeather(FAtmosphericSnapshot& S, const FCigiWeatherState& W);

	/** Canonical atmosphere/weather commands → the CIGI state the fog and cloud code reads (field for field). */
	static FCigiAtmosphereState ToAtmosphereState(const FAtmosphereCommand& A);
	static FCigiWeatherState    ToWeatherState(const FWeatherCommand& W);

	/** Control API (ROADMAP 2.4): apply as a CIGI Atmosphere Control would. Game thread. */
	void ApplyAtmosphereCommand(const FAtmosphereCommand& A);
	/** Control API (ROADMAP 2.4): one global weather layer, as a CIGI Weather Control with Region ID 0 would. Game thread. */
	void ApplyWeatherCommand(const FWeatherCommand& W);

	/** Thread-safe snapshot (written on game thread, read by ACamSimCamera same game thread). */
	FAtmosphericSnapshot GetAtmosphericSnapshot() const { return CachedAtmosSnapshot; }

	/**
	 * Whether a Celestial Sphere Control differs from the last one applied (Prev null: none yet): date/time, Date Valid,
	 * Ephemeris, Sun/Moon/Star Field Enable and Star Field Intensity (two NaN intensities are equal). Hosts may send the
	 * packet every frame; only a change moves the clock and re-applies the sky.
	 */
	static bool IsCelestialChange(const FCigiCelestialState* Prev, const FCigiCelestialState& Next);
	/** The part of IsCelestialChange that moves the sim clock: date/time, Date Valid, Ephemeris Enable. An enable or
	 *  intensity change alone only re-applies the sky (it must not rewind the clock to the packet's hh:mm). */
	static bool IsCelestialClockChange(const FCigiCelestialState* Prev, const FCigiCelestialState& Next);
	/** Whether the tick re-applies the sky (ephemeris, lights, illumination): never applied (Last 0), the sim clock moved
	 *  back or by a second or more, or the observer (camera) moved more than SkyObserverMoveM since the last apply. The
	 *  ephemeris is per observer (LAST, moon parallax), and the clock can stand still while the camera moves (lockstep,
	 *  a static CIGI time). */
	static bool ShouldReapplySky(uint64 NowMicros, uint64 LastMicros, double Lat, double Lon, double LastLat, double LastLon);
	/** 1 km: 0.01 deg of local sidereal time at the equator. */
	static constexpr double SkyObserverMoveM = 1000.0;

#if WITH_DEV_AUTOMATION_TESTS
	/** Test hook (CamSim.Environment.Sky.*): sets the sky up if BeginPlay didn't (no subsystem in a test world) and applies
	 *  it for Utc at the georeference origin. */
	void ApplySkyForTest(const FDateTime& Utc);
	/** Test hook: a Celestial Sphere Control against Clock (sim time rate 1 without a subsystem). */
	void ApplyCelestialControlForTest(const FCigiCelestialState& Cel, FSimClock& Clock) { ApplyCelestialControl(Cel, Clock); }
#endif

private:
	// Cached UE environment actors (found via TActorIterator in BeginPlay). The sun light, sky light and atmosphere are
	// CesiumSunSky's components (ROADMAP 6A); FallbackSun only without a CesiumSunSky (test levels).
	UPROPERTY(Transient) TObjectPtr<ADirectionalLight>     FallbackSun;
	UPROPERTY(Transient) TObjectPtr<UDirectionalLightComponent> SunLightComp;
	UPROPERTY(Transient) TObjectPtr<AExponentialHeightFog> HeightFog;
	UPROPERTY(Transient) TObjectPtr<AVolumetricCloud>      CloudActor;
	UPROPERTY(Transient) TObjectPtr<ACesiumSunSky>         CesiumSunSkyActor;
	UPROPERTY(Transient) TObjectPtr<ACesiumGeoreference>   Georeference;

	// ROADMAP 6A night sky
	UPROPERTY(Transient) TObjectPtr<UDirectionalLightComponent> MoonLight;   // atmosphere light index 1
	UPROPERTY(Transient) FSkyDomeDriver SkyDome;
	FCachedExposureBand Band;
	FSkyRecaptureHoldState SkyRecaptureHold;
	FCamSimConfig::FCelestialConfig CelestialCfg;   // copied at setup (defaults in a test world)
	FEphemerisResult LastEphem;
	FNightIlluminationResult LastIllum;
	FCelestialEnables LastEnables;
	double LastSkyCenturiesJ2000 = 0.0;   // the last ApplySky's time (star precession)
	bool bSkySetUp = false;
	bool bSkyApplied = false;

	UPROPERTY(Transient) TObjectPtr<UCamSimSubsystem> Subsystem;
	UPROPERTY(Transient) TObjectPtr<ACamSimCamera>    CamSimCameraActor;

	// Latest state from CIGI queues
	FCigiCelestialState  LastCelestial;   // last Celestial Sphere Control applied to the sim clock
	FCigiAtmosphereState CurrentAtmosphere;
	FCigiWeatherState    CurrentWeather;

	bool bReceivedCelestial  = false;
	bool bReceivedAtmosphere = false;
	bool bReceivedWeather    = false;

	float SunElevationDeg  = 0.0f;   // current
	uint64 LastSunMicros   = 0;      // sim time the sun was last applied (0 = now)
	double LastSkyLat = 0.0, LastSkyLon = 0.0;   // the observer of the last sky apply

	// Cached atmospheric snapshot (Phase 18) — updated each time atmosphere/weather changes
	FAtmosphericSnapshot CachedAtmosSnapshot;

	// Phase 18 config (copied from FCamSimConfig at BeginPlay)
	FCamSimConfig::FPhase18Config Phase18Cfg;

	// Ocean (ROADMAP 2.6)
	FOceanManager OceanManager;

	/** CIGI Celestial Sphere Control → sim clock date/time and rate (on a clock change) and the celestial enables. */
	void ApplyCelestialControl(const FCigiCelestialState& Cel, FSimClock& Clock);
	/** Lights and moon for a UTC time (ROADMAP 6A): ephemeris, illumination, light intensities and visibility. The
	 *  directions and MPC_Sky follow in ApplySkyFrame (every tick). */
	void ApplySky(const FDateTime& Utc);
	/** Every tick: sun/moon/star directions in the current UE frame (origin shifts rotate it), moon light rotation, dome at the
	 *  camera, MPC_Sky. Uses the last ApplySky's ephemeris. */
	void ApplySkyFrame();
	/** Finds CesiumSunSky's components (or a fallback sun), creates the moon light and the dome. Idempotent. */
	void SetUpSky();
	/** Lumen cached-lighting exposure band from the EO AE's scene median (every tick). */
	void UpdateCachedExposureBand();
	/** The observer: camera telemetry, else the configured start point, else the georeference origin. HAE metres. */
	void GetObserver(double& OutLat, double& OutLon, double& OutAltM) const;
	void ApplyAtmosphere();
	void ApplyWeather();

	// Phase 18 helpers
	void ApplySecondFogLayer();                              // 18C
	void SetCloudShadowsEnabled(bool bEnabled, float Strength); // 18B
};
