// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CIGI/CigiPacketTypes.h"
#include "Config/CamSimConfig.h"
#include "Ocean/FOceanManager.h"
#include "CamSimEnvironment.generated.h"

class ADirectionalLight;
class ASkyLight;
class ASkyAtmosphere;
class AExponentialHeightFog;
class AVolumetricCloud;
class ACesiumSunSky;
class UCamSimSubsystem;
class ACamSimCamera;

/** Weather parameters for a regional zone (blended when camera is inside the radius). */
struct FWeatherZoneParams
{
	float FogDensity    = 0.0f;
	float VisibilityM   = 10000.0f;
	float CloudCoverage = 0.0f;
};

/** Runtime state of a weather zone — position from YAML config, params from CIGI. */
struct FWeatherZone
{
	int32  ZoneID  = 0;
	double LatDeg  = 0.0;
	double LonDeg  = 0.0;
	float  RadiusM = 10000.0f;
	FWeatherZoneParams Params;
};

/**
 * ACamSimEnvironment
 *
 * Drives UE5 environment actors (sun, sky, fog, clouds) from CIGI
 * Celestial Sphere Control, Atmosphere Control, and Weather Control packets.
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
	virtual void Tick(float DeltaTime) override;

	/** Current sun elevation in degrees above horizon (Phase 16K). */
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
	};

	/** Visibility below which the atmosphere counts as fog (thermal path extinction, ROADMAP 4A). */
	static constexpr float FogVisibilityM = 10000.0f;

	/** CIGI Atmosphere Control → snapshot: air temperature (finite only), visibility (finite, > 0), humidity, fog. */
	static void FoldAtmosphere(FAtmosphericSnapshot& S, const FCigiAtmosphereState& A);
	/** CIGI Weather Control (global) → snapshot cloud cover (coverage % / 100, clamped; non-finite → 0). */
	static void FoldWeather(FAtmosphericSnapshot& S, const FCigiWeatherState& W);

	/** Thread-safe snapshot (written on game thread, read by ACamSimCamera same game thread). */
	FAtmosphericSnapshot GetAtmosphericSnapshot() const { return CachedAtmosSnapshot; }

	/** Called from ACamSimCamera::Tick() to update camera position for zone blending (18L). */
	void SetCameraPosition(double LatDeg, double LonDeg);

private:
	// Cached UE environment actors (found via TActorIterator in BeginPlay)
	UPROPERTY(Transient) TObjectPtr<ADirectionalLight>    SunLight;
	UPROPERTY(Transient) TObjectPtr<ASkyLight>            SkyLight;
	UPROPERTY(Transient) TObjectPtr<ASkyAtmosphere>       SkyAtmosphere;
	UPROPERTY(Transient) TObjectPtr<AExponentialHeightFog> HeightFog;
	UPROPERTY(Transient) TObjectPtr<AVolumetricCloud>      CloudActor;
	UPROPERTY(Transient) TObjectPtr<ACesiumSunSky>         CesiumSunSkyActor;

	UPROPERTY(Transient) TObjectPtr<UCamSimSubsystem> Subsystem;
	UPROPERTY(Transient) TObjectPtr<ACamSimCamera>    CamSimCameraActor;

	// Latest state from CIGI queues
	FCigiCelestialState  LastCelestial;   // last Celestial Sphere Control applied to the sim clock
	FCigiAtmosphereState CurrentAtmosphere;
	FCigiWeatherState    CurrentWeather;

	bool bReceivedCelestial  = false;
	bool bReceivedAtmosphere = false;
	bool bReceivedWeather    = false;

	// Previous sun elevation for sky-light recapture hysteresis
	float PrevSunElevation = 0.0f;   // at the last sky-light recapture
	float SunElevationDeg  = 0.0f;   // current
	uint64 LastSunMicros   = 0;      // sim time the sun was last applied (0 = now)

	// Cached atmospheric snapshot (Phase 18) — updated each time atmosphere/weather changes
	FAtmosphericSnapshot CachedAtmosSnapshot;

	// Phase 18 config (copied from FCamSimConfig at BeginPlay)
	FCamSimConfig::FPhase18Config Phase18Cfg;

	// Ocean (ROADMAP 2.6)
	FOceanManager OceanManager;

	// 18L: Weather zone blending
	TArray<FWeatherZone>  ActiveWeatherZones;    // runtime zones (up to 16)
	FWeatherZoneParams    GlobalWeatherBaseline; // fog density from last global weather update
	double                CameraLatDeg = 0.0;   // updated each tick via SetCameraPosition()
	double                CameraLonDeg = 0.0;

	/** CIGI Celestial Sphere Control → sim clock date/time and rate. */
	void ApplyCelestialControl(const FCigiCelestialState& Cel);
	/** Sun position and lighting for a UTC time. */
	void ApplySun(const FDateTime& Utc);
	void ApplyAtmosphere();
	void ApplyWeather();

	// Phase 18 helpers
	void ApplySecondFogLayer();                              // 18C
	void ApplyGodRays();                                     // 18E
	void ApplySkyAtmosphericScattering();                    // 18J
	void SetCloudShadowsEnabled(bool bEnabled, float Strength); // 18B

	// 18L: Weather zone blending helpers
	void  UpdateWeatherZone(uint16 RegionId, const FWeatherZoneParams& Params);
	void  BlendWeatherZones();
	static float GreatCircleApproxM(double Lat1, double Lon1, double Lat2, double Lon2);

};
