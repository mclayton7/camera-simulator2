// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * FCamSimTelemetry
 *
 * Per-frame telemetry snapshot used throughout CamSim:
 *   - FKlvBuilder consumes it to produce MISB ST 0601 KLV (Metadata/KlvBuilder.h).
 *   - FHudOverlay reads it for on-screen text (Overlay/FHudOverlay.h).
 *   - FCotSender / FMultiViewFrameSink reference it for telemetry passthrough.
 *
 * Phase 5: lifted out of KlvBuilder.h so consumers that need the telemetry struct
 * don't have to transitively include the entire KLV builder API. KlvBuilder.h
 * still includes this header so existing KLV callers compile unchanged.
 */
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

	// Phase 26: additional ST 0601.9 fields
	float  GroundSpeedMps  = 0.0f; // metres/sec — Tag 8 (0 = omit)

	// Active sensor state snapshot (for optional sidecar ground-truth output).
	uint8 SensorMode      = 0;    // 0=EO, 1=IR, 2=NVG
	uint8 SensorPolarity  = 0;    // 0=white-hot, 1=black-hot (IR)

	// Environment state for sensor effects (Phase 16K sun glint).
	float SunElevationDeg = 45.0f; // degrees above horizon (negative = below)

	// Atmospheric snapshot for Phase 18 sensor + KLV annotation.
	float AtmosphericVisibilityM = 10000.0f; // metres (meteorological visibility)
	float RelativeHumidity       = 0.5f;     // [0,1]
	float AirTempCelsius         = 15.0f;    // degrees Celsius
	float WeatherSeverity        = 0.0f;     // [0,1] — 0=clear, 1=severe
	uint8 WeatherPrecipType      = 0;        // 0=none, 1=rain, 2=snow
};
