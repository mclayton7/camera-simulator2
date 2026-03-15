// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DIS/DisPduTypes.h"
#include "CIGI/CigiPacketTypes.h"

struct FCamSimConfig;
class FDisReceiver;

/**
 * FDisEntityAdapter
 *
 * Game-thread component that bridges DIS protocol to the CamSim entity pipeline.
 *
 * Each game tick it:
 *   1. Drains FDisReceiver's SPSC queue of FDisEntityStatePdu
 *   2. Translates DIS Entity IDs to CamSim uint16 IDs
 *   3. Converts ECEF → WGS-84 geodetic coordinates
 *   4. Converts DIS orientation (radians, NED) → CIGI convention (degrees)
 *   5. Maps DIS DR algorithms to FCigiRateControl
 *   6. Maps DIS entity types to CamSim type IDs
 *   7. Sweeps for timed-out entities and emits Remove states
 *
 * Output: FCigiEntityState + FCigiRateControl structs, consumed by
 * FCamSimEntityManager via dequeue accessors.
 */
class FDisEntityAdapter
{
public:
	explicit FDisEntityAdapter(const FCamSimConfig& InConfig, FDisReceiver* InReceiver);

	/**
	 * Process pending DIS PDUs and timeout checks.
	 * Called once per game tick from UCamSimSubsystem or FCamSimEntityManager.
	 */
	void Tick(float DeltaTime);

	/** Dequeue a converted entity state. Returns false if empty. */
	bool DequeueEntityState(FCigiEntityState& Out);

	/** Dequeue a converted rate control. Returns false if empty. */
	bool DequeueRateControl(FCigiRateControl& Out);

	// -----------------------------------------------------------------------
	// Static utility: ECEF → geodetic conversion (double precision)
	// -----------------------------------------------------------------------

	/**
	 * Convert ECEF (Earth-Centered, Earth-Fixed) coordinates to geodetic.
	 * Uses iterative Bowring method for sub-centimetre accuracy.
	 *
	 * @param X, Y, Z  ECEF position in metres.
	 * @param OutLat   WGS-84 latitude in decimal degrees.
	 * @param OutLon   WGS-84 longitude in decimal degrees.
	 * @param OutAlt   Height above WGS-84 ellipsoid in metres.
	 */
	static void EcefToGeodetic(double X, double Y, double Z,
	                           double& OutLat, double& OutLon, double& OutAlt);

	// -----------------------------------------------------------------------
	// ID Translation
	// -----------------------------------------------------------------------

	/**
	 * Get or allocate a CamSim uint16 ID for a DIS entity ID triple.
	 * IDs start at DisIdBaseOffset (default 1000) to avoid collision with CIGI IDs.
	 */
	uint16 GetOrAllocateId(const FDisEntityId& DisId);

	/** Release a CamSim ID back to the free list. */
	void ReleaseId(const FDisEntityId& DisId);

	// -----------------------------------------------------------------------
	// Entity Type Mapping
	// -----------------------------------------------------------------------

	/**
	 * Look up CamSim entity type ID for a DIS entity type.
	 * Falls back through: exact match → fuzzy match → default.
	 */
	uint16 MapEntityType(const FDisEntityType& DisType) const;

private:
	const FCamSimConfig& Config;
	FDisReceiver*        Receiver = nullptr;

	// Output queues (game-thread — adapter produces, entity manager consumes)
	TArray<FCigiEntityState> PendingEntityStates;
	TArray<FCigiRateControl> PendingRateControls;
	int32 EntityDrainIndex = 0;
	int32 RateDrainIndex   = 0;

	// ID translation: DIS triple → CamSim uint16
	TMap<FDisEntityId, uint16>   DisIdMap;
	TArray<uint16>               FreeIds;
	uint16                       NextId = 0;

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
	uint16                DefaultEntityTypeId = 1001;

	void ProcessPdu(const FDisEntityStatePdu& Pdu);
	void SweepTimeouts();
	void BuildTypeMaps();
};
