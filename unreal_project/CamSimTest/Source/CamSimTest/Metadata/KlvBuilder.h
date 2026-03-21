// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * FCamSimTelemetry
 *
 * Per-frame telemetry snapshot consumed by FKlvBuilder to produce MISB ST 0601 KLV.
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

/**
 * FKlvBuilder
 *
 * Static helper (with one-time configuration) that encodes an FCamSimTelemetry into a MISB ST 0601
 * KLV Local Set byte buffer ready to be written to an FFmpeg data-stream packet.
 *
 * Tags implemented (ST 0601.9, ascending order):
 *   Tag  1  – Checksum (CRC-16/CCITT or BCC-16, configurable)
 *   Tag  2  – UNIX Time Stamp            (uint64, μs, 8 bytes)
 *   Tag  4  – Platform Tail Number       (ISO 646 string, configurable)
 *   Tag  5  – Platform Heading Angle     (uint16, 0..360°)
 *   Tag  6  – Platform Pitch Angle       (int16,  ±20°)
 *   Tag  7  – Platform Roll Angle        (int16,  ±50°)
 *   Tag  8  – Platform Ground Speed      (uint8, 0..255 m/s)
 *   Tag 11  – Image Source Sensor        (ISO 646 string: "EO Nose" / "LWIR" / "NVG")
 *   Tag 12  – Image Coordinate System    (ISO 646 string: "Geodetic WGS84")
 *   Tag 13  – Sensor Latitude            (int32,  ±90°)
 *   Tag 14  – Sensor Longitude           (int32,  ±180°)
 *   Tag 15  – Sensor True Altitude       (uint16, −900..19000 m)
 *   Tag 16  – Sensor Horizontal FOV      (uint16, 0..180°)
 *   Tag 17  – Sensor Vertical FOV        (uint16, 0..180°)
 *   Tag 18  – Sensor Relative Azimuth    (uint32, 0..360°, gimbal yaw)
 *   Tag 19  – Sensor Relative Elevation  (int32,  ±180°, gimbal pitch)
 *   Tag 20  – Sensor Relative Roll       (uint32, 0..360°, gimbal roll)
 *   Tag 21  – Slant Range                (uint32, 0..5 000 000 m)
 *   Tag 23  – Frame Center Latitude      (int32,  ±90°)
 *   Tag 24  – Frame Center Longitude     (int32,  ±180°)
 *   Tag 25  – Frame Center Elevation     (uint16, −900..19000 m)
 *   Tag 40  – Target Track Gate Width    (uint8, pixels, configurable)
 *   Tag 41  – Target Track Gate Height   (uint8, pixels, configurable)
 *   Tag 47  – Generic Flag Data 01       (uint8 bitmask: bit5=IR polarity, bit3=range valid)
 *   Tag 65  – UAS LS Version Number      (uint8, value=9)
 *
 * Checksum: default is CRC-16/CCITT (poly 0x1021, init 0xFFFF) to match validate_klv.py.
 * Configure() can switch to BCC-16 (running 16-bit modular sum) per ST 0601 spec.
 */
class FKlvBuilder
{
public:
	/**
	 * Build a complete MISB ST 0601 Local Set KLV packet.
	 * The returned buffer starts with the 16-byte Universal Label key,
	 * followed by a BER-OID length, followed by tag-length-value triplets,
	 * ending with the CRC-16 checksum (Tag 1).
	 *
	 * If security metadata has been initialised via SetSecurityMetadata(),
	 * Tag 48 (Security Local Metadata Set / ST 0102) is included automatically.
	 */
	static TArray<uint8> BuildMisbST0601(const FCamSimTelemetry& Telemetry);

	/**
	 * Initialise the static ST 0102 security metadata payload.
	 * Called once at startup from UCamSimSubsystem::Initialize().
	 * The pre-built TLV content is embedded as ST 0601 Tag 48 in every
	 * subsequent call to BuildMisbST0601().
	 */
	static void SetSecurityMetadata(const FString& Classification,
	                                const FString& ClassifyingCountry,
	                                const FString& ObjectCountryCodes,
	                                const FString& Caveats = FString(),
	                                const FString& ReleasingInstructions = FString());

	/**
	 * Phase 26: one-time configuration for checksum algorithm, tail number,
	 * and target track gate dimensions. Called at startup from subsystem init.
	 */
	static void Configure(const FString& ChecksumAlgo,
	                       const FString& TailNumber,
	                       float TargetTrackGateWidth,
	                       float TargetTrackGateHeight);

	// -----------------------------------------------------------------------
	// Public helpers used by the tag-descriptor table in KlvBuilder.cpp.
	// These pure-math converters are also useful for testing individual tags.
	// -----------------------------------------------------------------------
	static void AppendTag(TArray<uint8>& Buf, uint8 Tag, const uint8* Value, uint8 Len);

	// MISB fixed-point mapping helpers
	static int32  MapLatLon(double Degrees, double Range); // signed, 4-byte, ±Range°
	static uint32 MapAzimuth360(float Degrees);            // unsigned, 4-byte, 0..360°
	static int32  MapElevation180(float Degrees);          // signed,   4-byte, ±180°
	static int16  MapAltitude(double Metres);              // uint16 as int16, −900..19000 m
	static uint16 MapFov(float Degrees);                   // unsigned, 2-byte, 0..180°
	static uint16 MapHeading(float Degrees);               // unsigned, 2-byte, 0..360°
	static int16  MapPlatformPitch(float Degrees);         // signed,   2-byte, ±20°
	static int16  MapPlatformRoll(float Degrees);          // signed,   2-byte, ±50°
	static uint32 MapSlantRange(double Metres);            // unsigned, 4-byte, 0..5000000 m
	static uint8  MapGroundSpeed(float MetresPerSec);      // unsigned, 1-byte, 0..255 m/s

	// Checksum helpers (public for testing)
	static uint16 ComputeCrc16(const uint8* Data, int32 Len);
	static uint16 ComputeBcc16(const uint8* Data, int32 Len);
};
