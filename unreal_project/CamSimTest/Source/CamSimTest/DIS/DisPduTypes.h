// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// DIS IEEE 1278.1 PDU type codes
namespace EDisPduType
{
	constexpr uint8 EntityState = 1;
	constexpr uint8 Fire        = 2;
	constexpr uint8 Detonation  = 3;
	constexpr uint8 Designator  = 24;
}

// DIS protocol family codes
namespace EDisProtocolFamily
{
	constexpr uint8 EntityInformation = 1;
	constexpr uint8 Warfare          = 2;
}

/**
 * DIS Entity Identifier — 48-bit composite key {Site, Application, Entity}.
 * Used as map key for ID translation.
 */
struct FDisEntityId
{
	uint16 Site        = 0;
	uint16 Application = 0;
	uint16 Entity      = 0;

	bool operator==(const FDisEntityId& Other) const
	{
		return Site == Other.Site && Application == Other.Application && Entity == Other.Entity;
	}

	bool operator!=(const FDisEntityId& Other) const { return !(*this == Other); }

	friend uint32 GetTypeHash(const FDisEntityId& Id)
	{
		// Combine all three fields into a single hash
		return HashCombine(HashCombine(::GetTypeHash(Id.Site), ::GetTypeHash(Id.Application)),
		                   ::GetTypeHash(Id.Entity));
	}

	FString ToString() const
	{
		return FString::Printf(TEXT("%u:%u:%u"), Site, Application, Entity);
	}
};

/**
 * DIS Entity Type — 7-field enumeration from IEEE 1278.1.
 * Maps to entity model selection via YAML config.
 */
struct FDisEntityType
{
	uint8  EntityKind   = 0;  // 1=Platform, 2=Munition, 3=Lifeform, ...
	uint8  Domain       = 0;  // 1=Land, 2=Air, 3=Surface, 4=Subsurface, 5=Space
	uint16 Country      = 0;  // SISO-REF-010
	uint8  Category     = 0;
	uint8  Subcategory  = 0;
	uint8  Specific     = 0;
	uint8  Extra        = 0;

	FString ToString() const
	{
		return FString::Printf(TEXT("%u:%u:%u:%u:%u:%u:%u"),
			EntityKind, Domain, Country, Category, Subcategory, Specific, Extra);
	}
};

/**
 * DIS Dead Reckoning Parameters — embedded in Entity State PDU.
 */
struct FDisDeadReckoningParams
{
	uint8 Algorithm = 0;  // DR algorithm code (1-9)

	// Linear velocity (m/s) in entity body coordinates
	float VelX = 0.0f;
	float VelY = 0.0f;
	float VelZ = 0.0f;

	// Linear acceleration (m/s^2) in entity body coordinates
	float AccelX = 0.0f;
	float AccelY = 0.0f;
	float AccelZ = 0.0f;

	// Angular velocity (rad/s) in entity body coordinates
	float AngVelX = 0.0f;  // roll rate
	float AngVelY = 0.0f;  // pitch rate
	float AngVelZ = 0.0f;  // yaw rate
};

/**
 * FDisPduHeader
 *
 * 12-byte DIS PDU header (IEEE 1278.1).
 * All multi-byte fields are big-endian (network byte order).
 */
struct FDisPduHeader
{
	uint8  ProtocolVersion = 0;  // 6 = DIS 6 (IEEE 1278.1-2012), 7 = DIS 7
	uint8  ExerciseId      = 0;
	uint8  PduType         = 0;  // EDisPduType
	uint8  ProtocolFamily  = 0;  // EDisProtocolFamily
	uint32 Timestamp       = 0;
	uint16 PduLength       = 0;  // total PDU length in bytes (header + body)
	uint16 Padding         = 0;

	/**
	 * Parse a 12-byte DIS PDU header from a raw buffer.
	 * @param Data    Pointer to at least 12 bytes of PDU data.
	 * @param OutHdr  Parsed header (host byte order).
	 * @return true if parsing succeeded (buffer has valid structure).
	 */
	static bool Parse(const uint8* Data, int32 DataLen, FDisPduHeader& OutHdr);
};

/**
 * FDisEntityStatePdu
 *
 * DIS Entity State PDU (type 1) — 144 bytes.
 * Position is ECEF (metres), orientation is Euler angles (radians, NED body-to-world).
 */
struct FDisEntityStatePdu
{
	FDisPduHeader       Header;
	FDisEntityId        EntityId;
	uint8               ForceId = 0;  // 0=other, 1=friendly, 2=opposing, 3=neutral

	FDisEntityType      EntityType;
	FDisEntityType      AlternativeEntityType;

	// Entity location in ECEF (metres from Earth center) — double precision
	double LocationX = 0.0;
	double LocationY = 0.0;
	double LocationZ = 0.0;

	// Entity orientation — Euler angles (radians), body-to-world, NED frame
	// Psi = heading/yaw, Theta = pitch, Phi = roll
	float Psi   = 0.0f;
	float Theta = 0.0f;
	float Phi   = 0.0f;

	// Dead reckoning parameters
	FDisDeadReckoningParams DeadReckoning;

	// Entity marking (11 chars ASCII + encoding byte)
	FString Marking;

	// Entity appearance (32-bit bitfield — platform-specific)
	uint32 Appearance = 0;

	/**
	 * Parse a complete Entity State PDU from raw bytes.
	 * @param Data    Pointer to the full PDU (including header).
	 * @param DataLen Length of the buffer in bytes.
	 * @param OutPdu  Parsed PDU in host byte order.
	 * @return true if parsing succeeded and PDU type is EntityState.
	 */
	static bool Parse(const uint8* Data, int32 DataLen, FDisEntityStatePdu& OutPdu);
};
