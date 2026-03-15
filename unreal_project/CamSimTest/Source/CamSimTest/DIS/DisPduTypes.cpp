// Copyright CamSim Contributors. All Rights Reserved.

#include "DIS/DisPduTypes.h"

// DIS uses big-endian (network byte order) for all multi-byte fields.
// UE's ByteSwap.h or platform ntoh* may not be available everywhere,
// so we define portable inline helpers.
namespace
{

FORCEINLINE uint16 ReadU16BE(const uint8* P)
{
	return static_cast<uint16>((P[0] << 8) | P[1]);
}

FORCEINLINE uint32 ReadU32BE(const uint8* P)
{
	return (static_cast<uint32>(P[0]) << 24)
	     | (static_cast<uint32>(P[1]) << 16)
	     | (static_cast<uint32>(P[2]) << 8)
	     |  static_cast<uint32>(P[3]);
}

FORCEINLINE float ReadF32BE(const uint8* P)
{
	const uint32 Bits = ReadU32BE(P);
	float Val;
	FMemory::Memcpy(&Val, &Bits, 4);
	return Val;
}

FORCEINLINE uint64 ReadU64BE(const uint8* P)
{
	return (static_cast<uint64>(ReadU32BE(P)) << 32) | static_cast<uint64>(ReadU32BE(P + 4));
}

FORCEINLINE double ReadF64BE(const uint8* P)
{
	const uint64 Bits = ReadU64BE(P);
	double Val;
	FMemory::Memcpy(&Val, &Bits, 8);
	return Val;
}

} // anonymous namespace

// -------------------------------------------------------------------------
// FDisPduHeader::Parse
// -------------------------------------------------------------------------

bool FDisPduHeader::Parse(const uint8* Data, int32 DataLen, FDisPduHeader& OutHdr)
{
	if (!Data || DataLen < 12) return false;

	OutHdr.ProtocolVersion = Data[0];
	OutHdr.ExerciseId      = Data[1];
	OutHdr.PduType         = Data[2];
	OutHdr.ProtocolFamily  = Data[3];
	OutHdr.Timestamp       = ReadU32BE(Data + 4);
	OutHdr.PduLength       = ReadU16BE(Data + 8);
	OutHdr.Padding         = ReadU16BE(Data + 10);

	return true;
}

// -------------------------------------------------------------------------
// FDisEntityStatePdu::Parse
//
// Entity State PDU layout (144 bytes total):
//   Offset  0: PDU Header (12 bytes)
//   Offset 12: Entity ID (6 bytes: site, app, entity — each uint16)
//   Offset 18: Force ID (1 byte)
//   Offset 19: Num Articulation Params (1 byte)
//   Offset 20: Entity Type (8 bytes: kind, domain, country, cat, subcat, specific, extra)
//   Offset 28: Alternative Entity Type (8 bytes)
//   Offset 36: Linear Velocity (12 bytes: 3x float32 — entity body frame, m/s)
//   Offset 48: Entity Location (24 bytes: 3x float64 — ECEF metres)
//   Offset 72: Entity Orientation (12 bytes: 3x float32 — psi/theta/phi radians)
//   Offset 84: Entity Appearance (4 bytes: uint32)
//   Offset 88: Dead Reckoning Parameters (40 bytes)
//       88: DR algorithm (1 byte)
//       89: Other params (15 bytes — padding/reserved in most impls)
//      104: Entity Linear Acceleration (12 bytes: 3x float32)
//      116: Entity Angular Velocity (12 bytes: 3x float32)
//   Offset 128: Entity Marking (12 bytes: 1 encoding + 11 ASCII)
//   Offset 140: Capabilities (4 bytes)
//   Total: 144 bytes (before articulation parameters)
// -------------------------------------------------------------------------

bool FDisEntityStatePdu::Parse(const uint8* Data, int32 DataLen, FDisEntityStatePdu& OutPdu)
{
	// Minimum Entity State PDU size (no articulation params)
	if (!Data || DataLen < 144) return false;

	// Parse header
	if (!FDisPduHeader::Parse(Data, DataLen, OutPdu.Header)) return false;
	if (OutPdu.Header.PduType != EDisPduType::EntityState) return false;

	// Entity ID (offset 12)
	OutPdu.EntityId.Site        = ReadU16BE(Data + 12);
	OutPdu.EntityId.Application = ReadU16BE(Data + 14);
	OutPdu.EntityId.Entity      = ReadU16BE(Data + 16);

	// Force ID (offset 18)
	OutPdu.ForceId = Data[18];

	// Entity Type (offset 20)
	OutPdu.EntityType.EntityKind  = Data[20];
	OutPdu.EntityType.Domain      = Data[21];
	OutPdu.EntityType.Country     = ReadU16BE(Data + 22);
	OutPdu.EntityType.Category    = Data[24];
	OutPdu.EntityType.Subcategory = Data[25];
	OutPdu.EntityType.Specific    = Data[26];
	OutPdu.EntityType.Extra       = Data[27];

	// Alternative Entity Type (offset 28)
	OutPdu.AlternativeEntityType.EntityKind  = Data[28];
	OutPdu.AlternativeEntityType.Domain      = Data[29];
	OutPdu.AlternativeEntityType.Country     = ReadU16BE(Data + 30);
	OutPdu.AlternativeEntityType.Category    = Data[32];
	OutPdu.AlternativeEntityType.Subcategory = Data[33];
	OutPdu.AlternativeEntityType.Specific    = Data[34];
	OutPdu.AlternativeEntityType.Extra       = Data[35];

	// Linear Velocity (offset 36) — used for DR, stored in DeadReckoning
	OutPdu.DeadReckoning.VelX = ReadF32BE(Data + 36);
	OutPdu.DeadReckoning.VelY = ReadF32BE(Data + 40);
	OutPdu.DeadReckoning.VelZ = ReadF32BE(Data + 44);

	// Entity Location ECEF (offset 48) — double precision
	OutPdu.LocationX = ReadF64BE(Data + 48);
	OutPdu.LocationY = ReadF64BE(Data + 56);
	OutPdu.LocationZ = ReadF64BE(Data + 64);

	// Entity Orientation (offset 72) — radians, NED body-to-world
	OutPdu.Psi   = ReadF32BE(Data + 72);  // heading/yaw
	OutPdu.Theta = ReadF32BE(Data + 76);  // pitch
	OutPdu.Phi   = ReadF32BE(Data + 80);  // roll

	// Entity Appearance (offset 84)
	OutPdu.Appearance = ReadU32BE(Data + 84);

	// Dead Reckoning Parameters (offset 88)
	OutPdu.DeadReckoning.Algorithm = Data[88];
	// Bytes 89-103: other DR parameters (padding/reserved) — skip
	// Linear Acceleration (offset 104)
	OutPdu.DeadReckoning.AccelX = ReadF32BE(Data + 104);
	OutPdu.DeadReckoning.AccelY = ReadF32BE(Data + 108);
	OutPdu.DeadReckoning.AccelZ = ReadF32BE(Data + 112);
	// Angular Velocity (offset 116)
	OutPdu.DeadReckoning.AngVelX = ReadF32BE(Data + 116);
	OutPdu.DeadReckoning.AngVelY = ReadF32BE(Data + 120);
	OutPdu.DeadReckoning.AngVelZ = ReadF32BE(Data + 124);

	// Entity Marking (offset 128): 1 byte encoding + 11 bytes ASCII
	// Encoding byte at 128 (6 = ASCII); marking text at 129-139
	const int32 MarkingLen = 11;
	ANSICHAR MarkingBuf[12];
	FMemory::Memcpy(MarkingBuf, Data + 129, MarkingLen);
	MarkingBuf[MarkingLen] = '\0';
	OutPdu.Marking = FString(ANSI_TO_TCHAR(MarkingBuf)).TrimEnd();

	// Capabilities (offset 140) — 4 bytes, currently unused

	return true;
}

// -------------------------------------------------------------------------
// FDisDesignatorPdu::Parse
//
// Designator PDU layout (minimum 56 bytes):
//   Offset  0: PDU Header (12 bytes)
//   Offset 12: Designating Entity ID (6 bytes)
//   Offset 18: Code Name (2 bytes)
//   Offset 20: Designated Entity ID (6 bytes)
//   Offset 26: Designator Code (2 bytes)
//   Offset 28: Designator Power (4 bytes, float32)
//   Offset 32: Spot Location ECEF (24 bytes: 3x float64)
// -------------------------------------------------------------------------

bool FDisDesignatorPdu::Parse(const uint8* Data, int32 DataLen, FDisDesignatorPdu& Out)
{
	if (!Data || DataLen < 56) return false;

	if (!FDisPduHeader::Parse(Data, DataLen, Out.Header)) return false;
	if (Out.Header.PduType != EDisPduType::Designator) return false;

	// Designating Entity ID (offset 12)
	Out.DesignatingEntityId.Site        = ReadU16BE(Data + 12);
	Out.DesignatingEntityId.Application = ReadU16BE(Data + 14);
	Out.DesignatingEntityId.Entity      = ReadU16BE(Data + 16);

	// Code Name (offset 18)
	Out.CodeName = ReadU16BE(Data + 18);

	// Designated Entity ID (offset 20)
	Out.DesignatedEntityId.Site        = ReadU16BE(Data + 20);
	Out.DesignatedEntityId.Application = ReadU16BE(Data + 22);
	Out.DesignatedEntityId.Entity      = ReadU16BE(Data + 24);

	// Designator Code (offset 26)
	Out.DesignatorCode = ReadU16BE(Data + 26);

	// Designator Power (offset 28)
	Out.DesignatorPower = ReadF32BE(Data + 28);

	// Spot Location ECEF (offset 32)
	Out.SpotLocationX = ReadF64BE(Data + 32);
	Out.SpotLocationY = ReadF64BE(Data + 40);
	Out.SpotLocationZ = ReadF64BE(Data + 48);

	return true;
}
