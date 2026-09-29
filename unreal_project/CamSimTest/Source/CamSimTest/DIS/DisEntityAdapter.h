// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DIS/DisPduTypes.h"

struct FCamSimConfig;
class FDisReceiver;
class ISimCommandSink;

/**
 * FDisEntityAdapter
 *
 * The DIS host adapter (passive "stealth viewer"). Each frame it drains
 * FDisReceiver's Entity State PDUs and submits canonical entity commands
 * (Hosts/DisCommands.h: ECEF → geodetic and local attitude, dead-reckoning
 * motion model), keyed by the DIS entity ID in the DIS namespace; maps DIS
 * entity types to CamSim types; removes entities that time out; and tracks
 * the latest laser designator spot.
 */
class FDisEntityAdapter
{
public:
	explicit FDisEntityAdapter(const FCamSimConfig& InConfig, FDisReceiver* InReceiver);

	/**
	 * Drain received PDUs and submit entity commands (with dead-reckoning
	 * motion); remove entities that stopped sending. Game thread, once a frame.
	 */
	void Poll(ISimCommandSink& Sink);

	/** One Entity State PDU → entity command (public for tests). */
	void ProcessPdu(const FDisEntityStatePdu& Pdu, ISimCommandSink& Sink);



	// -----------------------------------------------------------------------
	// Entity Type Mapping
	// -----------------------------------------------------------------------

	/**
	 * Look up CamSim entity type ID for a DIS entity type. Falls back through:
	 * 1. Exact match (kind:domain:country:category:subcategory:specific:extra)
	 * 2. Category fuzzy match (kind:domain:category)
	 * 3. Domain fallback (kind:domain)
	 * 4. Default (DefaultEntityTypeId)
	 */
	uint16 MapEntityType(const FDisEntityType& DisType) const;

	/**
	 * Get the latest designator spot in geodetic coordinates.
	 * Returns false if no designator is active (timed out or never received).
	 */
	bool GetDesignatorSpot(double& OutLat, double& OutLon, double& OutAlt, int32& OutCode) const;

private:
	const FCamSimConfig& Config;
	FDisReceiver*        Receiver = nullptr;

	// Entity timeout tracking
	struct FEntityTimestamp
	{
		FDisEntityId DisId;
		double       LastUpdateSec = 0.0;
	};
	TMap<FDisEntityId, FEntityTimestamp> EntityTimestamps;

	// Entity type mapping cache (populated from config)
	// Key: "kind:domain:country:category:subcategory:specific:extra"
	TMap<FString, uint16> ExactTypeMap;
	// Fuzzy key: "kind:domain:category"
	TMap<FString, uint16> FuzzyTypeMap;
	// Domain fallback key: "kind:domain"
	TMap<FString, uint16> DomainTypeMap;
	// DomainTypeMap keys whose current winner has subcategory 0 (generic).
	TSet<FString>          DomainGeneric;
	uint16                DefaultEntityTypeId = 1001;

	// Designator spot state (Phase 21F.2)
	bool   bDesignatorActive = false;
	double DesignatorLat = 0.0;
	double DesignatorLon = 0.0;
	double DesignatorAlt = 0.0;
	int32  DesignatorCode = 0;
	double DesignatorUpdateTimeSec = 0.0;

	void DrainDesignatorPdus();


	void SweepTimeouts(ISimCommandSink& Sink);
	void BuildTypeMaps();
};
