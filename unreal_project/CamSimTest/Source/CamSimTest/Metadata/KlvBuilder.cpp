// Copyright CamSim Contributors. All Rights Reserved.

#include "Metadata/KlvBuilder.h"
#include "CamSimTest.h"

// -------------------------------------------------------------------------
// MISB ST 0601 Universal Label (16 bytes)
// "Motion Imagery Standards Board" Local Set key
// -------------------------------------------------------------------------
static const uint8 kST0601_UL[16] = {
	0x06, 0x0E, 0x2B, 0x34,
	0x02, 0x0B, 0x01, 0x01,
	0x0E, 0x01, 0x03, 0x01,
	0x01, 0x00, 0x00, 0x00
};

// -------------------------------------------------------------------------
// Cached MISB ST 0102 Security Local Set payload (Phase 12A)
//
// Initialised once via FKlvBuilder::SetSecurityMetadata() at startup.
// If non-empty, embedded as ST 0601 Tag 48 in every KLV packet.
// -------------------------------------------------------------------------
static TArray<uint8> CachedST0102Payload;

// -------------------------------------------------------------------------
// Phase 26: configurable checksum algorithm + cached config statics
// -------------------------------------------------------------------------
enum class EKlvChecksumAlgo : uint8 { CRC16, BCC16 };
static EKlvChecksumAlgo GKlvChecksumAlgo = EKlvChecksumAlgo::CRC16;
static TArray<uint8> GCachedTailNumberAnsi;
static uint8  GCachedTargetGateWidth  = 0;
static uint8  GCachedTargetGateHeight = 0;

// -------------------------------------------------------------------------
// Tag descriptor table
//
// Each entry describes one TLV field.  The encoder lambda appends the
// encoded bytes for that tag into the supplied Value buffer.
// Tags are written in ascending tag-number order (ST 0601 requirement).
// -------------------------------------------------------------------------

struct FKlvTagDescriptor
{
	uint8 Tag;
	TFunction<void(TArray<uint8>&, const FCamSimTelemetry&)> Encode;
};

// Append a 4-byte signed lat/lon fixed-point tag
static void AppendLatLon4(TArray<uint8>& V, uint8 Tag, double Degrees, double Range)
{
	int32 Mapped = FKlvBuilder::MapLatLon(Degrees, Range);
	uint8 Tmp[4] = {
		uint8((Mapped >> 24) & 0xFF), uint8((Mapped >> 16) & 0xFF),
		uint8((Mapped >>  8) & 0xFF), uint8(Mapped & 0xFF)
	};
	FKlvBuilder::AppendTag(V, Tag, Tmp, 4);
}

// Append a variable-length ISO 646 string tag
static void AppendStringTag(TArray<uint8>& V, uint8 Tag, const char* Str)
{
	const uint8 Len = static_cast<uint8>(FCStringAnsi::Strlen(Str));
	FKlvBuilder::AppendTag(V, Tag, reinterpret_cast<const uint8*>(Str), Len);
}

static const TArray<FKlvTagDescriptor> KlvTagTable = {
	// Tag 2 – UNIX Time Stamp, 8-byte unsigned, microseconds
	{ 2, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		uint8 Tmp[8];
		uint64 Ts = T.TimestampUs;
		for (int i = 7; i >= 0; --i) { Tmp[i] = Ts & 0xFF; Ts >>= 8; }
		FKlvBuilder::AppendTag(V, 2, Tmp, 8);
	}},

	// Tag 4 – Platform Tail Number, ISO 646 string (Phase 26A)
	{ 4, [](TArray<uint8>& V, const FCamSimTelemetry&)
	{
		if (GCachedTailNumberAnsi.Num() == 0) return;
		FKlvBuilder::AppendTag(V, 4, GCachedTailNumberAnsi.GetData(),
			static_cast<uint8>(GCachedTailNumberAnsi.Num()));
	}},

	// Tag 5 – Platform Heading Angle, 2-byte unsigned, 0..360°
	{ 5, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		uint16 H = FKlvBuilder::MapHeading(T.Yaw);
		uint8 Tmp[2] = { uint8((H >> 8) & 0xFF), uint8(H & 0xFF) };
		FKlvBuilder::AppendTag(V, 5, Tmp, 2);
	}},

	// Tag 6 – Platform Pitch Angle, 2-byte signed, ±20°
	{ 6, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		int16 P = FKlvBuilder::MapPlatformPitch(T.Pitch);
		uint8 Tmp[2] = { uint8((P >> 8) & 0xFF), uint8(P & 0xFF) };
		FKlvBuilder::AppendTag(V, 6, Tmp, 2);
	}},

	// Tag 7 – Platform Roll Angle, 2-byte signed, ±50°
	{ 7, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		int16 R = FKlvBuilder::MapPlatformRoll(T.Roll);
		uint8 Tmp[2] = { uint8((R >> 8) & 0xFF), uint8(R & 0xFF) };
		FKlvBuilder::AppendTag(V, 7, Tmp, 2);
	}},

	// Tag 8 – Platform Ground Speed, 1-byte unsigned, 0..255 m/s (Phase 26C)
	{ 8, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		if (T.GroundSpeedMps <= 0.0f) return;
		uint8 Spd = FKlvBuilder::MapGroundSpeed(T.GroundSpeedMps);
		FKlvBuilder::AppendTag(V, 8, &Spd, 1);
	}},

	// Tag 11 – Image Source Sensor, ISO 646 string
	{ 11, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		const char* Name = (T.SensorMode == 1) ? "LWIR"
		                 : (T.SensorMode == 2) ? "NVG"
		                 :                       "EO Nose";
		AppendStringTag(V, 11, Name);
	}},

	// Tag 12 – Image Coordinate System, ISO 646 string (static)
	{ 12, [](TArray<uint8>& V, const FCamSimTelemetry&)
	{
		AppendStringTag(V, 12, "Geodetic WGS84");
	}},

	// Tag 13 – Sensor Latitude, 4-byte signed ±90°
	{ 13, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		AppendLatLon4(V, 13, T.Latitude, 90.0);
	}},

	// Tag 14 – Sensor Longitude, 4-byte signed ±180°
	{ 14, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		AppendLatLon4(V, 14, T.Longitude, 180.0);
	}},

	// Tag 15 – Sensor True Altitude, 2-byte unsigned −900..19000 m
	{ 15, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		const uint16 Alt = static_cast<uint16>(FKlvBuilder::MapAltitude(T.Altitude));
		uint8 Tmp[2] = { uint8((Alt >> 8) & 0xFF), uint8(Alt & 0xFF) };
		FKlvBuilder::AppendTag(V, 15, Tmp, 2);
	}},

	// Tag 16 – Sensor Horizontal FOV, 2-byte unsigned 0..180°
	{ 16, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		uint16 Fov = FKlvBuilder::MapFov(T.HFovDeg);
		uint8 Tmp[2] = { uint8((Fov >> 8) & 0xFF), uint8(Fov & 0xFF) };
		FKlvBuilder::AppendTag(V, 16, Tmp, 2);
	}},

	// Tag 17 – Sensor Vertical FOV, 2-byte unsigned 0..180°
	{ 17, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		uint16 Fov = FKlvBuilder::MapFov(T.VFovDeg > 0.0f ? T.VFovDeg : T.HFovDeg * (9.0f / 16.0f));
		uint8 Tmp[2] = { uint8((Fov >> 8) & 0xFF), uint8(Fov & 0xFF) };
		FKlvBuilder::AppendTag(V, 17, Tmp, 2);
	}},

	// Tag 18 – Sensor Relative Azimuth Angle, 4-byte unsigned 0..360° (gimbal yaw)
	{ 18, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		uint32 Az = FKlvBuilder::MapAzimuth360(T.GimbalYaw);
		uint8 Tmp[4] = {
			uint8((Az >> 24) & 0xFF), uint8((Az >> 16) & 0xFF),
			uint8((Az >>  8) & 0xFF), uint8(Az & 0xFF)
		};
		FKlvBuilder::AppendTag(V, 18, Tmp, 4);
	}},

	// Tag 19 – Sensor Relative Elevation Angle, 4-byte signed ±180° (gimbal pitch)
	{ 19, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		int32 El = FKlvBuilder::MapElevation180(T.GimbalPitch);
		uint8 Tmp[4] = {
			uint8((El >> 24) & 0xFF), uint8((El >> 16) & 0xFF),
			uint8((El >>  8) & 0xFF), uint8(El & 0xFF)
		};
		FKlvBuilder::AppendTag(V, 19, Tmp, 4);
	}},

	// Tag 20 – Sensor Relative Roll Angle, 4-byte unsigned 0..360° (gimbal roll)
	{ 20, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		uint32 Ro = FKlvBuilder::MapAzimuth360(T.GimbalRoll);
		uint8 Tmp[4] = {
			uint8((Ro >> 24) & 0xFF), uint8((Ro >> 16) & 0xFF),
			uint8((Ro >>  8) & 0xFF), uint8(Ro & 0xFF)
		};
		FKlvBuilder::AppendTag(V, 20, Tmp, 4);
	}},

	// Tag 21 – Slant Range, 4-byte unsigned 0..5000000 m (omit if 0)
	{ 21, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		if (T.SlantRangeM <= 0.0) return;
		uint32 SR = FKlvBuilder::MapSlantRange(T.SlantRangeM);
		uint8 Tmp[4] = {
			uint8((SR >> 24) & 0xFF), uint8((SR >> 16) & 0xFF),
			uint8((SR >>  8) & 0xFF), uint8(SR & 0xFF)
		};
		FKlvBuilder::AppendTag(V, 21, Tmp, 4);
	}},

	// Tag 23 – Frame Center Latitude, 4-byte signed ±90°
	{ 23, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		AppendLatLon4(V, 23, T.FrameCenterLat, 90.0);
	}},

	// Tag 24 – Frame Center Longitude, 4-byte signed ±180°
	{ 24, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		AppendLatLon4(V, 24, T.FrameCenterLon, 180.0);
	}},

	// Tag 25 – Frame Center Elevation, 2-byte unsigned −900..19000 m
	{ 25, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		const uint16 Alt = static_cast<uint16>(FKlvBuilder::MapAltitude(T.FrameCenterElev));
		uint8 Tmp[2] = { uint8((Alt >> 8) & 0xFF), uint8(Alt & 0xFF) };
		FKlvBuilder::AppendTag(V, 25, Tmp, 2);
	}},

	// Tag 40 – Target Track Gate Width, 1-byte unsigned, pixels (Phase 26D)
	{ 40, [](TArray<uint8>& V, const FCamSimTelemetry&)
	{
		if (GCachedTargetGateWidth == 0) return;
		FKlvBuilder::AppendTag(V, 40, &GCachedTargetGateWidth, 1);
	}},

	// Tag 41 – Target Track Gate Height, 1-byte unsigned, pixels (Phase 26D)
	{ 41, [](TArray<uint8>& V, const FCamSimTelemetry&)
	{
		if (GCachedTargetGateHeight == 0) return;
		FKlvBuilder::AppendTag(V, 41, &GCachedTargetGateHeight, 1);
	}},

	// Tag 47 – Generic Flag Data 01, 1-byte bitmask
	//   Bit 5 (0x20): IR Polarity — 1 = black-hot
	//   Bit 3 (0x08): Slant Range valid — 1 = range is computed
	{ 47, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		uint8 Flags = 0;
		if (T.SensorPolarity == 1) Flags |= 0x20;
		if (T.SlantRangeM   >  0) Flags |= 0x08;
		FKlvBuilder::AppendTag(V, 47, &Flags, 1);
	}},

	// Tag 48 – Security Local Metadata Set (MISB ST 0102, Phase 12A)
	// Nested TLV content built by FKlvBuilder::SetSecurityMetadata().
	{ 48, [](TArray<uint8>& V, const FCamSimTelemetry&)
	{
		if (CachedST0102Payload.Num() == 0) return;
		// Tag 48 may exceed 127 bytes — use BER long-form length if needed
		V.Add(48);
		const int32 PayloadLen = CachedST0102Payload.Num();
		if (PayloadLen < 128)
		{
			V.Add(static_cast<uint8>(PayloadLen));
		}
		else
		{
			V.Add(0x81);
			V.Add(static_cast<uint8>(PayloadLen));
		}
		V.Append(CachedST0102Payload);
	}},

	// Tag 65 – UAS LS Version Number, 1-byte unsigned, value=9 (ST 0601.9)
	{ 65, [](TArray<uint8>& V, const FCamSimTelemetry&)
	{
		constexpr uint8 Version = 9;
		FKlvBuilder::AppendTag(V, 65, &Version, 1);
	}},
};

// -------------------------------------------------------------------------
// FKlvBuilder::BuildMisbST0601
// -------------------------------------------------------------------------

TArray<uint8> FKlvBuilder::BuildMisbST0601(const FCamSimTelemetry& T)
{
	// Build value payload by iterating the descriptor table
	TArray<uint8> Value;
	Value.Reserve(200);

	for (const FKlvTagDescriptor& Desc : KlvTagTable)
	{
		Desc.Encode(Value, T);
	}

	// Reserve 4 bytes for CRC tag (written after CRC computation)
	constexpr int32 CrcTagLen = 4;  // tag(1) + len(1) + crc(2)

	// Assemble full packet: UL key + BER length + TLV payload + CRC tag
	TArray<uint8> Packet;
	Packet.Reserve(16 + 3 + Value.Num() + CrcTagLen);

	Packet.Append(kST0601_UL, 16);

	const int32 TotalValueLen = Value.Num() + CrcTagLen;
	if (TotalValueLen < 128)
	{
		Packet.Add(static_cast<uint8>(TotalValueLen));
	}
	else if (TotalValueLen < 256)
	{
		Packet.Add(0x81);
		Packet.Add(static_cast<uint8>(TotalValueLen));
	}
	else
	{
		Packet.Add(0x82);
		Packet.Add(static_cast<uint8>((TotalValueLen >> 8) & 0xFF));
		Packet.Add(static_cast<uint8>(TotalValueLen & 0xFF));
	}

	Packet.Append(Value);

	// Checksum over everything so far (UL + length + TLVs)
	const uint16 Checksum = (GKlvChecksumAlgo == EKlvChecksumAlgo::BCC16)
		? ComputeBcc16(Packet.GetData(), Packet.Num())
		: ComputeCrc16(Packet.GetData(), Packet.Num());

	Packet.Add(1);   // Tag 1 (checksum)
	Packet.Add(2);   // length
	Packet.Add(static_cast<uint8>((Checksum >> 8) & 0xFF));
	Packet.Add(static_cast<uint8>(Checksum & 0xFF));

	return Packet;
}

// -------------------------------------------------------------------------
// Public encoding helpers (used by the tag table lambdas above)
// -------------------------------------------------------------------------

void FKlvBuilder::AppendTag(TArray<uint8>& Buf, uint8 Tag, const uint8* Value, uint8 Len)
{
	Buf.Add(Tag);
	Buf.Add(Len);
	Buf.Append(Value, Len);
}

// -------------------------------------------------------------------------
// MISB fixed-point mapping
// -------------------------------------------------------------------------

int32 FKlvBuilder::MapLatLon(double Degrees, double Range)
{
	if (!FMath::IsFinite(Degrees)) Degrees = 0.0;
	const double Scale   = static_cast<double>(0x7FFFFFFF) / Range;
	const double Clamped = FMath::Clamp(Degrees, -Range, Range);
	return static_cast<int32>(FMath::RoundToInt(Clamped * Scale));
}

// Tag 18/20: Sensor Relative Azimuth/Roll — unsigned uint32, 0..360°
uint32 FKlvBuilder::MapAzimuth360(float Degrees)
{
	if (!FMath::IsFinite(Degrees)) Degrees = 0.0f;
	// Normalize to [0, 360) then map to full uint32 range
	const double Norm = FMath::Fmod(static_cast<double>(Degrees) + 360.0, 360.0);
	return static_cast<uint32>(Norm / 360.0 * static_cast<double>(TNumericLimits<uint32>::Max()));
}

// Tag 19: Sensor Relative Elevation — signed int32, ±180°
int32 FKlvBuilder::MapElevation180(float Degrees)
{
	if (!FMath::IsFinite(Degrees)) Degrees = 0.0f;
	const double Scale   = static_cast<double>(0x7FFFFFFF) / 180.0;
	const double Clamped = FMath::Clamp(static_cast<double>(Degrees), -180.0, 180.0);
	return static_cast<int32>(FMath::RoundToInt(Clamped * Scale));
}

// Tags 15/25: Altitude, 2-byte unsigned mapped from −900..19000 m
int16 FKlvBuilder::MapAltitude(double Metres)
{
	if (!FMath::IsFinite(Metres)) Metres = 0.0;
	constexpr double MinAlt = -900.0;
	constexpr double MaxAlt = 19000.0;
	const double Scale      = 65535.0 / (MaxAlt - MinAlt);
	const double Clamped    = FMath::Clamp(Metres, MinAlt, MaxAlt);
	return static_cast<int16>(static_cast<uint16>(FMath::RoundToInt((Clamped - MinAlt) * Scale)));
}

// Tags 16/17: FOV, 2-byte unsigned, 0..180°
uint16 FKlvBuilder::MapFov(float Degrees)
{
	if (!FMath::IsFinite(Degrees)) Degrees = 0.0f;
	const double Scale   = 65535.0 / 180.0;
	const float  Clamped = FMath::Clamp(Degrees, 0.0f, 180.0f);
	return static_cast<uint16>(FMath::RoundToInt(static_cast<double>(Clamped) * Scale));
}

// Tag 5: Platform Heading, 2-byte unsigned, 0..360°
uint16 FKlvBuilder::MapHeading(float Degrees)
{
	if (!FMath::IsFinite(Degrees)) Degrees = 0.0f;
	const double Norm = FMath::Fmod(static_cast<double>(Degrees) + 360.0, 360.0);
	return static_cast<uint16>(Norm / 360.0 * 65535.0);
}

// Tag 6: Platform Pitch, 2-byte signed, ±20°
int16 FKlvBuilder::MapPlatformPitch(float Degrees)
{
	if (!FMath::IsFinite(Degrees)) Degrees = 0.0f;
	const double Scale   = static_cast<double>(0x7FFF) / 20.0;
	const double Clamped = FMath::Clamp(static_cast<double>(Degrees), -20.0, 20.0);
	return static_cast<int16>(FMath::RoundToInt(Clamped * Scale));
}

// Tag 7: Platform Roll, 2-byte signed, ±50°
int16 FKlvBuilder::MapPlatformRoll(float Degrees)
{
	if (!FMath::IsFinite(Degrees)) Degrees = 0.0f;
	const double Scale   = static_cast<double>(0x7FFF) / 50.0;
	const double Clamped = FMath::Clamp(static_cast<double>(Degrees), -50.0, 50.0);
	return static_cast<int16>(FMath::RoundToInt(Clamped * Scale));
}

// Tag 21: Slant Range, 4-byte unsigned, 0..5000000 m
uint32 FKlvBuilder::MapSlantRange(double Metres)
{
	if (!FMath::IsFinite(Metres)) Metres = 0.0;
	constexpr double Max     = 5000000.0;
	const double     Clamped = FMath::Clamp(Metres, 0.0, Max);
	return static_cast<uint32>(Clamped / Max * static_cast<double>(TNumericLimits<uint32>::Max()));
}

// -------------------------------------------------------------------------
// SetSecurityMetadata — build cached MISB ST 0102 nested TLV payload
//
// ST 0102 tags (nested inside ST 0601 Tag 48 as raw TLV pairs):
//   Tag  1 – Security Classification  (1 byte enum)
//   Tag  2 – CC/RI Coding Method      (1 byte, 1=ISO-3166 Two Letter)
//   Tag  3 – Classifying Country      (variable string, e.g. "//US")
//   Tag  5 – Caveats                  (variable string, optional)
//   Tag  6 – Releasing Instructions   (variable string, optional)
//   Tag 12 – Object Country Codes     (variable string, e.g. "US")
//   Tag 22 – ST 0102 Version Number   (2 bytes, uint16, value=12)
// -------------------------------------------------------------------------

static uint8 ClassificationStringToLevel(const FString& Cls)
{
	const FString Lower = Cls.ToLower().TrimStartAndEnd();
	if (Lower == TEXT("restricted"))   return 2;
	if (Lower == TEXT("confidential")) return 3;
	if (Lower == TEXT("secret"))       return 4;
	if (Lower == TEXT("top secret") || Lower == TEXT("topsecret")) return 5;
	return 1; // UNCLASSIFIED
}

void FKlvBuilder::SetSecurityMetadata(const FString& Classification,
                                      const FString& ClassifyingCountry,
                                      const FString& ObjectCountryCodes,
                                      const FString& Caveats,
                                      const FString& ReleasingInstructions)
{
	CachedST0102Payload.Reset();

	// Tag 1 – Security Classification (1 byte)
	const uint8 Level = ClassificationStringToLevel(Classification);
	AppendTag(CachedST0102Payload, 1, &Level, 1);

	// Tag 2 – CC/RI Coding Method (1 byte, 1 = ISO-3166 Two Letter)
	constexpr uint8 CcMethod = 1;
	AppendTag(CachedST0102Payload, 2, &CcMethod, 1);

	// Tag 3 – Classifying Country (variable string)
	{
		auto Ansi = StringCast<ANSICHAR>(*ClassifyingCountry);
		AppendTag(CachedST0102Payload, 3,
		          reinterpret_cast<const uint8*>(Ansi.Get()),
		          static_cast<uint8>(FMath::Min(Ansi.Length(), 255)));
	}

	// Tag 5 – Caveats (optional)
	if (!Caveats.IsEmpty())
	{
		auto Ansi = StringCast<ANSICHAR>(*Caveats);
		AppendTag(CachedST0102Payload, 5,
		          reinterpret_cast<const uint8*>(Ansi.Get()),
		          static_cast<uint8>(FMath::Min(Ansi.Length(), 255)));
	}

	// Tag 6 – Releasing Instructions (optional)
	if (!ReleasingInstructions.IsEmpty())
	{
		auto Ansi = StringCast<ANSICHAR>(*ReleasingInstructions);
		AppendTag(CachedST0102Payload, 6,
		          reinterpret_cast<const uint8*>(Ansi.Get()),
		          static_cast<uint8>(FMath::Min(Ansi.Length(), 255)));
	}

	// Tag 12 – Object Country Codes (variable string)
	{
		auto Ansi = StringCast<ANSICHAR>(*ObjectCountryCodes);
		AppendTag(CachedST0102Payload, 12,
		          reinterpret_cast<const uint8*>(Ansi.Get()),
		          static_cast<uint8>(FMath::Min(Ansi.Length(), 255)));
	}

	// Tag 22 – ST 0102 Version Number (2 bytes, uint16 = 12)
	constexpr uint16 Version = 12;
	uint8 VerBuf[2] = { uint8((Version >> 8) & 0xFF), uint8(Version & 0xFF) };
	AppendTag(CachedST0102Payload, 22, VerBuf, 2);

	UE_LOG(LogCamSim, Log,
		TEXT("FKlvBuilder: ST 0102 security metadata initialised (%d bytes, classification=%s country=%s)"),
		CachedST0102Payload.Num(), *Classification, *ClassifyingCountry);
}

// -------------------------------------------------------------------------
// CRC-16/CCITT (polynomial 0x1021, initial value 0xFFFF)
// -------------------------------------------------------------------------

uint16 FKlvBuilder::ComputeCrc16(const uint8* Data, int32 Len)
{
	uint16 Crc = 0xFFFF;
	for (int32 i = 0; i < Len; ++i)
	{
		Crc ^= static_cast<uint16>(Data[i]) << 8;
		for (int b = 0; b < 8; ++b)
		{
			if (Crc & 0x8000)
				Crc = static_cast<uint16>((Crc << 1) ^ 0x1021);
			else
				Crc <<= 1;
		}
	}
	return Crc;
}

// -------------------------------------------------------------------------
// BCC-16: running 16-bit modular sum (ST 0601 spec checksum)
// Even-indexed bytes accumulate into the high byte, odd into the low byte.
// NOTE: This matches the common JMISB / impleotv interpretation. Verify
// against the MISB ST 0601.9 §12 reference vector if strict compliance is
// required. CRC-16 (the default) is recommended for most integrations.
// -------------------------------------------------------------------------

uint16 FKlvBuilder::ComputeBcc16(const uint8* Data, int32 Len)
{
	uint16 Bcc = 0;
	for (int32 i = 0; i < Len; ++i)
	{
		Bcc += static_cast<uint16>(Data[i]) << ((i & 1) ? 0 : 8);
	}
	return Bcc;
}

// -------------------------------------------------------------------------
// MapGroundSpeed — 1-byte unsigned, 0..255 m/s (Tag 8)
// -------------------------------------------------------------------------

uint8 FKlvBuilder::MapGroundSpeed(float MetresPerSec)
{
	if (!FMath::IsFinite(MetresPerSec)) MetresPerSec = 0.0f;
	return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(MetresPerSec), 0, 255));
}

// -------------------------------------------------------------------------
// Phase 26: Configure — set checksum algorithm and cached tag values
// -------------------------------------------------------------------------

void FKlvBuilder::Configure(const FString& ChecksumAlgo,
                             const FString& TailNumber,
                             float TargetTrackGateWidth,
                             float TargetTrackGateHeight)
{
	const FString AlgoLower = ChecksumAlgo.ToLower();
	if (AlgoLower == TEXT("bcc16"))
	{
		GKlvChecksumAlgo = EKlvChecksumAlgo::BCC16;
	}
	else
	{
		if (AlgoLower != TEXT("crc16") && !AlgoLower.IsEmpty())
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("FKlvBuilder::Configure: unknown checksum '%s', defaulting to crc16"), *ChecksumAlgo);
		}
		GKlvChecksumAlgo = EKlvChecksumAlgo::CRC16;
	}

	// Pre-convert tail number to ANSI bytes (avoids per-frame StringCast allocation)
	GCachedTailNumberAnsi.Reset();
	if (!TailNumber.IsEmpty())
	{
		auto Ansi = StringCast<ANSICHAR>(*TailNumber);
		const int32 Len = FMath::Min(Ansi.Length(), 127);
		GCachedTailNumberAnsi.Append(reinterpret_cast<const uint8*>(Ansi.Get()), Len);
	}
	GCachedTargetGateWidth  = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(TargetTrackGateWidth),  0, 255));
	GCachedTargetGateHeight = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(TargetTrackGateHeight), 0, 255));

	UE_LOG(LogCamSim, Log,
		TEXT("FKlvBuilder: Phase 26 configured (checksum=%s, tail=%s, gate=%dx%d)"),
		GKlvChecksumAlgo == EKlvChecksumAlgo::BCC16 ? TEXT("bcc16") : TEXT("crc16"),
		TailNumber.IsEmpty() ? TEXT("(none)") : *TailNumber,
		GCachedTargetGateWidth, GCachedTargetGateHeight);
}
