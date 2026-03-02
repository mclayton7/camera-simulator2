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
// Tag descriptor table
//
// Each entry describes one TLV field.  The encoder lambda appends the
// encoded bytes for that tag into the supplied Value buffer.
// Tags are written in ascending tag-number order (ST 0601 requirement).
// Tags 26/27/28 are intentional duplicates of 20/21/22 for legacy consumers.
// -------------------------------------------------------------------------

struct FKlvTagDescriptor
{
	uint8 Tag;
	TFunction<void(TArray<uint8>&, const FCamSimTelemetry&)> Encode;
};

static void AppendLatLon4(TArray<uint8>& V, uint8 Tag, double Degrees, double Range)
{
	int32 Mapped = FKlvBuilder::MapLatLon(Degrees, Range);
	uint8 Tmp[4] = {
		uint8((Mapped >> 24) & 0xFF), uint8((Mapped >> 16) & 0xFF),
		uint8((Mapped >>  8) & 0xFF), uint8(Mapped & 0xFF)
	};
	FKlvBuilder::AppendTag(V, Tag, Tmp, 4);
}

static void AppendAngle360(TArray<uint8>& V, uint8 Tag, float Degrees)
{
	int32 Mapped = FKlvBuilder::MapAngle360(Degrees);
	uint8 Tmp[4] = {
		uint8((Mapped >> 24) & 0xFF), uint8((Mapped >> 16) & 0xFF),
		uint8((Mapped >>  8) & 0xFF), uint8(Mapped & 0xFF)
	};
	FKlvBuilder::AppendTag(V, Tag, Tmp, 4);
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

	// Tag 18 – Sensor Horizontal FOV, 2-byte unsigned 0..180°
	{ 18, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		uint16 Fov = FKlvBuilder::MapFov(T.HFovDeg);
		uint8 Tmp[2] = { uint8((Fov >> 8) & 0xFF), uint8(Fov & 0xFF) };
		FKlvBuilder::AppendTag(V, 18, Tmp, 2);
	}},

	// Tag 19 – Sensor Vertical FOV, 2-byte unsigned 0..180°
	{ 19, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		uint16 Fov = FKlvBuilder::MapFov(T.VFovDeg > 0.0f ? T.VFovDeg : T.HFovDeg * (9.0f / 16.0f));
		uint8 Tmp[2] = { uint8((Fov >> 8) & 0xFF), uint8(Fov & 0xFF) };
		FKlvBuilder::AppendTag(V, 19, Tmp, 2);
	}},

	// Tags 20/21/22 – Sensor Relative Azimuth/Elevation/Roll (gimbal vs platform)
	{ 20, [](TArray<uint8>& V, const FCamSimTelemetry& T) { AppendAngle360(V, 20, T.GimbalYaw);   }},
	{ 21, [](TArray<uint8>& V, const FCamSimTelemetry& T) { AppendAngle360(V, 21, T.GimbalPitch); }},
	{ 22, [](TArray<uint8>& V, const FCamSimTelemetry& T) { AppendAngle360(V, 22, T.GimbalRoll);  }},

	// Tag 23 – Slant Range, 8-byte uint64, IMAPB 0..5000000 m (omit if 0)
	{ 23, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		if (T.SlantRangeM <= 0.0) return;
		uint64 SR = FKlvBuilder::MapSlantRange(T.SlantRangeM);
		uint8 Tmp[8];
		for (int i = 7; i >= 0; --i) { Tmp[i] = SR & 0xFF; SR >>= 8; }
		FKlvBuilder::AppendTag(V, 23, Tmp, 8);
	}},

	// Tag 24 – Frame Center Latitude, 4-byte signed ±90°
	{ 24, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		AppendLatLon4(V, 24, T.FrameCenterLat, 90.0);
	}},

	// Tag 25 – Frame Center Longitude, 4-byte signed ±180°
	{ 25, [](TArray<uint8>& V, const FCamSimTelemetry& T)
	{
		AppendLatLon4(V, 25, T.FrameCenterLon, 180.0);
	}},

	// Tags 26/27/28 – duplicated gimbal angles for backwards-compat consumers
	{ 26, [](TArray<uint8>& V, const FCamSimTelemetry& T) { AppendAngle360(V, 26, T.GimbalYaw);   }},
	{ 27, [](TArray<uint8>& V, const FCamSimTelemetry& T) { AppendAngle360(V, 27, T.GimbalPitch); }},
	{ 28, [](TArray<uint8>& V, const FCamSimTelemetry& T) { AppendAngle360(V, 28, T.GimbalRoll);  }},
};

// -------------------------------------------------------------------------
// FKlvBuilder::BuildMisbST0601
// -------------------------------------------------------------------------

TArray<uint8> FKlvBuilder::BuildMisbST0601(const FCamSimTelemetry& T)
{
	// Build value payload by iterating the descriptor table
	TArray<uint8> Value;
	Value.Reserve(160);

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

	// CRC-16/CCITT over everything so far (UL + length + TLVs)
	const uint16 Crc = ComputeCrc16(Packet.GetData(), Packet.Num());

	Packet.Add(1);   // Tag 1 (checksum)
	Packet.Add(2);   // length
	Packet.Add(static_cast<uint8>((Crc >> 8) & 0xFF));
	Packet.Add(static_cast<uint8>(Crc & 0xFF));

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
	const double Scale   = static_cast<double>(0x7FFFFFFF) / Range;
	const double Clamped = FMath::Clamp(Degrees, -Range, Range);
	return static_cast<int32>(FMath::RoundToInt(Clamped * Scale));
}

int32 FKlvBuilder::MapAngle360(float Degrees)
{
	const double Scale   = static_cast<double>(0x7FFFFFFF) / 360.0;
	const double Clamped = FMath::Clamp(static_cast<double>(Degrees), -360.0, 360.0);
	return static_cast<int32>(FMath::RoundToInt(Clamped * Scale));
}

int16 FKlvBuilder::MapAltitude(double Metres)
{
	constexpr double MinAlt = -900.0;
	constexpr double MaxAlt = 19000.0;
	const double Scale      = 65535.0 / (MaxAlt - MinAlt);
	const double Clamped    = FMath::Clamp(Metres, MinAlt, MaxAlt);
	return static_cast<int16>(static_cast<uint16>(FMath::RoundToInt((Clamped - MinAlt) * Scale)));
}

uint16 FKlvBuilder::MapFov(float Degrees)
{
	const double Scale   = 65535.0 / 180.0;
	const float  Clamped = FMath::Clamp(Degrees, 0.0f, 180.0f);
	return static_cast<uint16>(FMath::RoundToInt(static_cast<double>(Clamped) * Scale));
}

uint64 FKlvBuilder::MapSlantRange(double Metres)
{
	constexpr double Max     = 5000000.0;
	const double     Clamped = FMath::Clamp(Metres, 0.0, Max);
	return static_cast<uint64>(Clamped / Max * static_cast<double>(TNumericLimits<uint64>::Max()));
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
