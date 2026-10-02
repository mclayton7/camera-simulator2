// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * FCamSimTelemetry
 *
 * Per-frame telemetry snapshot used throughout CamSim:
 *   - FKlvBuilder consumes it to produce MISB ST 0601 KLV (Metadata/KlvBuilder.h).
 *   - FMultiViewFrameSink references it for telemetry passthrough.
 *
 * Phase 5: lifted out of KlvBuilder.h so consumers that need the telemetry struct
 * don't have to transitively include the entire KLV builder API. KlvBuilder.h
 * still includes this header so existing KLV callers compile unchanged.
 */
namespace CamSimTelemetry
{
	/** Vertical FOV of a rectilinear image: 2·atan(tan(HFOV/2)·H/W), degrees (KLV Tag 17). */
	inline float VerticalFovDeg(float HFovDeg, int32 Width, int32 Height)
	{
		if (Width <= 0 || Height <= 0) return HFovDeg;
		const double HalfH = FMath::DegreesToRadians(0.5 * static_cast<double>(HFovDeg));
		return static_cast<float>(FMath::RadiansToDegrees(
			2.0 * FMath::Atan(FMath::Tan(HalfH) * static_cast<double>(Height) / static_cast<double>(Width))));
	}
}

struct FCamSimTelemetry
{
	uint64 TimestampUs = 0;    // POSIX microseconds (UTC)

	double Latitude    = 0.0;  // WGS-84 decimal degrees
	double Longitude   = 0.0;  // WGS-84 decimal degrees
	double Altitude    = 0.0;  // metres above WGS-84 ellipsoid

	float  Yaw         = 0.0f; // degrees [0, 360) — platform heading
	float  Pitch       = 0.0f; // degrees — platform elevation angle
	float  Roll        = 0.0f; // degrees — platform roll

	float  HFovDeg     = 60.0f; // degrees — horizontal field of view
	float  VFovDeg     = 33.75f;// degrees — vertical field of view (default 16:9)

	// Gimbal angles relative to platform body frame (0 = boresighted to platform)
	float  GimbalYaw   = 0.0f; // degrees — gimbal azimuth offset from platform heading
	float  GimbalPitch = 0.0f; // degrees — gimbal elevation offset
	float  GimbalRoll  = 0.0f; // degrees — gimbal roll offset

	// Geometric line-of-sight outputs (computed from platform pose + gimbal angles)
	double SlantRangeM     = 0.0;  // metres — Tag 21 (0 = sensor at/above horizon)
	double FrameCenterLat  = 0.0;  // WGS-84 decimal degrees — Tag 23
	double FrameCenterLon  = 0.0;  // WGS-84 decimal degrees — Tag 24
	double FrameCenterElev = 0.0;  // metres above ellipsoid  — Tag 25 (from terrain hit)

	// Image corners on the ground (Tags 82-89), ST 0601 order: 0 upper left, 1 upper
	// right, 2 lower right, 3 lower left. Bit i of CornerValidMask = corner i hit the
	// ground (terrain, or the ellipsoid at the frame-centre height); others are omitted.
	uint8  CornerValidMask = 0;
	double CornerLat[4]    = { 0.0, 0.0, 0.0, 0.0 };  // WGS-84 decimal degrees
	double CornerLon[4]    = { 0.0, 0.0, 0.0, 0.0 };

	// Phase 26: additional ST 0601.9 fields
	float  GroundSpeedMps  = 0.0f; // metres/sec — Tag 56 (0 = omit)

	// Platform kinematics from the host (CIGI user-defined packet 201); omitted when not valid.
	bool   bHasAirspeed         = false;
	float  TrueAirspeedMps      = 0.0f;  // Tag 8
	float  IndicatedAirspeedMps = 0.0f;  // Tag 9
	bool   bHasMagneticHeading  = false;
	float  MagneticHeadingDeg   = 0.0f;  // Tag 64
	bool   bHasVelocity         = false;
	float  VelNorthMps          = 0.0f;  // Tag 79
	float  VelEastMps           = 0.0f;  // Tag 80
	float  VelDownMps           = 0.0f;
	// Tag 2 is the host's sample time (packet 201) rather than the sim clock.
	bool   bTimestampFromHost   = false;

	// Active sensor state snapshot (for optional sidecar ground-truth output).
	uint8 SensorMode      = 0;    // 0=EO, 1=IR
	uint8 SensorPolarity  = 0;    // 0=white-hot, 1=black-hot (IR)

	// Environment state for sensor effects (Phase 16K sun glint).
	float SunElevationDeg = 45.0f; // degrees above horizon (negative = below)

	// Atmospheric snapshot for Phase 18 sensor + KLV annotation.
	float AtmosphericVisibilityM = 10000.0f; // metres (meteorological visibility)
	float RelativeHumidity       = 0.5f;     // [0,1] — Tag 55 (with bHasAtmosphere)
	float AirTempCelsius         = 15.0f;    // degrees Celsius — Tag 39 (with bHasAtmosphere)
	// CIGI Atmosphere Control from the host (Tags 35-37, 39, 55 are sent only once it has arrived).
	bool  bHasAtmosphere         = false;
	float WindDirectionDeg       = 0.0f;     // direction the wind blows from, true north — Tag 35
	float WindSpeedMps           = 0.0f;     // horizontal — Tag 36
	float BaroPressureMb         = 1013.25f; // sea-level pressure; Tag 37 reduces it to the platform altitude
	float WeatherSeverity        = 0.0f;     // [0,1] — 0=clear, 1=severe
	uint8 WeatherPrecipType      = 0;        // 0=none, 1=rain, 2=snow
};
