// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Metadata/CamSimTelemetry.h"

/**
 * FKlvBuilder
 *
 * Static helper (with one-time configuration) that encodes an FCamSimTelemetry into a MISB ST 0601
 * KLV Local Set byte buffer ready to be written to an FFmpeg data-stream packet.
 *
 * The reference decoder is misb.js (github.com/vidterra/misb.js): every tag
 * here must decode correctly with its st0601.parse().
 *
 * Tags implemented (ST 0601.9, ascending order):
 *   Tag  1  – Checksum                   (uint16 running sum, see ComputeChecksum)
 *   Tag  2  – UNIX Time Stamp            (uint64, μs, 8 bytes)
 *   Tag  4  – Platform Tail Number       (ISO 646 string, configurable)
 *   Tag  5  – Platform Heading Angle     (uint16, 0..360°)
 *   Tag  6  – Platform Pitch Angle       (int16,  ±20°)
 *   Tag  7  – Platform Roll Angle        (int16,  ±50°)
 *   Tag 11  – Image Source Sensor        (ISO 646 string: "EO Nose" / "LWIR" / "NVG")
 *   Tag 12  – Image Coordinate System    (ISO 646 string: "Geodetic WGS84")
 *   Tag 13  – Sensor Latitude            (int32,  ±90°)
 *   Tag 14  – Sensor Longitude           (int32,  ±180°)
 *   Tag 15  – Sensor True Altitude       (uint16, −900..19000 m MSL; omitted without the geoid grid)
 *   Tag 16  – Sensor Horizontal FOV      (uint16, 0..180°)
 *   Tag 17  – Sensor Vertical FOV        (uint16, 0..180°)
 *   Tag 18  – Sensor Relative Azimuth    (uint32, 0..360°, gimbal yaw)
 *   Tag 19  – Sensor Relative Elevation  (int32,  ±180°, gimbal pitch)
 *   Tag 20  – Sensor Relative Roll       (uint32, 0..360°, gimbal roll)
 *   Tag 21  – Slant Range                (uint32, 0..5 000 000 m)
 *   Tag 23  – Frame Center Latitude      (int32,  ±90°)
 *   Tag 24  – Frame Center Longitude     (int32,  ±180°)
 *   Tag 25  – Frame Center Elevation     (uint16, −900..19000 m MSL; omitted without the geoid grid)
 *   Tag 43  – Target Track Gate Width    (uint8, pixels / 2, configurable)
 *   Tag 44  – Target Track Gate Height   (uint8, pixels / 2, configurable)
 *   Tag 47  – Generic Flag Data          (uint8 bitmask: bit 3 = IR black-hot)
 *   Tag 48  – Security Local Set         (ST 0102, when configured)
 *   Tag 56  – Platform Ground Speed      (uint8, 0..255 m/s)
 *   Tag 65  – UAS LS Version Number      (uint8, value=9)
 *   Tag 75  – Sensor Ellipsoid Height    (uint16, −900..19000 m WGS-84)
 *   Tag 78  – Frame Center HAE           (uint16, −900..19000 m WGS-84)
 *
 * Telemetry altitudes are WGS-84 ellipsoid heights (Cesium's datum). MSL tags
 * subtract the EGM96 undulation from Geospatial/Geoid.h.
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
	 * In-place variant of BuildMisbST0601: fills OutPacket (after Reset()) so a
	 * single caller-owned buffer can be reused across frames without allocating
	 * a fresh TArray every time. Wire format is identical.
	 */
	static void BuildMisbST0601Into(const FCamSimTelemetry& Telemetry,
	                                TArray<uint8>& OutPacket);

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
	 * Phase 26: one-time configuration for tail number and target track gate
	 * dimensions (in pixels). Called at startup from subsystem init.
	 */
	static void Configure(const FString& TailNumber,
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
	static uint8  MapTrackGate(float Pixels);              // unsigned, 1-byte, pixels / 2

	// Tag 47 Generic Flag Data bits (bit 1 = LSB)
	static constexpr uint8 GenericFlagIrBlackHot = 0x04;   // bit 3

	/**
	 * ST 0601 checksum: running 16-bit sum over [Data, Data + Len). For a full
	 * packet, Len covers the UL key through the checksum item's "01 02" bytes.
	 */
	static uint16 ComputeChecksum(const uint8* Data, int32 Len);
};
