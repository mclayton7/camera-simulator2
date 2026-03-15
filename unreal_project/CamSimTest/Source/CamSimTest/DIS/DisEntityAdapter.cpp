// Copyright CamSim Contributors. All Rights Reserved.

#include "DIS/DisEntityAdapter.h"
#include "DIS/DisReceiver.h"
#include "Config/CamSimConfig.h"
#include "CamSimTest.h"

// WGS-84 ellipsoid constants (double precision)
namespace
{
	constexpr double WGS84_A  = 6378137.0;            // semi-major axis (m)
	constexpr double WGS84_B  = 6356752.314245;       // semi-minor axis (m)
	constexpr double WGS84_E2 = 0.00669437999014;     // first eccentricity squared
	constexpr double WGS84_EP2 = 0.00673949674228;    // second eccentricity squared
	constexpr double RAD_TO_DEG = 180.0 / PI;
}

// -------------------------------------------------------------------------
// Constructor
// -------------------------------------------------------------------------

FDisEntityAdapter::FDisEntityAdapter(const FCamSimConfig& InConfig, FDisReceiver* InReceiver)
	: Config(InConfig)
	, Receiver(InReceiver)
	, NextId(static_cast<uint16>(InConfig.DIS.IdBaseOffset))
{
	BuildTypeMaps();
}

// -------------------------------------------------------------------------
// Tick — drain DIS PDUs, convert, sweep timeouts
// -------------------------------------------------------------------------

void FDisEntityAdapter::Tick(float DeltaTime)
{
	if (!Receiver) return;

	// Reset output queues for this tick
	PendingEntityStates.Reset();
	PendingRateControls.Reset();
	EntityDrainIndex = 0;
	RateDrainIndex   = 0;

	// Drain all pending DIS PDUs
	FDisEntityStatePdu Pdu;
	while (Receiver->DequeueEntityStatePdu(Pdu))
	{
		ProcessPdu(Pdu);
	}

	// Sweep for timed-out entities
	SweepTimeouts();

	// Drain Designator PDUs (Phase 21F.2)
	DrainDesignatorPdus();
}

// -------------------------------------------------------------------------
// Dequeue accessors (consumed by FCamSimEntityManager)
// -------------------------------------------------------------------------

bool FDisEntityAdapter::DequeueEntityState(FCigiEntityState& Out)
{
	if (EntityDrainIndex >= PendingEntityStates.Num())
	{
		// All consumed — reset for next tick
		PendingEntityStates.Reset();
		EntityDrainIndex = 0;
		return false;
	}
	Out = PendingEntityStates[EntityDrainIndex++];
	return true;
}

bool FDisEntityAdapter::DequeueRateControl(FCigiRateControl& Out)
{
	if (RateDrainIndex >= PendingRateControls.Num())
	{
		PendingRateControls.Reset();
		RateDrainIndex = 0;
		return false;
	}
	Out = PendingRateControls[RateDrainIndex++];
	return true;
}

// -------------------------------------------------------------------------
// ProcessPdu — convert one DIS Entity State PDU to CamSim structs
// -------------------------------------------------------------------------

void FDisEntityAdapter::ProcessPdu(const FDisEntityStatePdu& Pdu)
{
	const double NowSec = FPlatformTime::Seconds();

	// Update timestamp for timeout tracking
	FEntityTimestamp& Ts = EntityTimestamps.FindOrAdd(Pdu.EntityId);
	Ts.DisId = Pdu.EntityId;
	Ts.LastUpdateSec = NowSec;

	// ID translation
	const uint16 CamSimId = GetOrAllocateId(Pdu.EntityId);

	// ECEF → geodetic
	double Lat, Lon, Alt;
	EcefToGeodetic(Pdu.LocationX, Pdu.LocationY, Pdu.LocationZ, Lat, Lon, Alt);

	// Build FCigiEntityState
	FCigiEntityState State;
	State.EntityId    = CamSimId;
	State.EntityState = 1;  // Active (DIS has no explicit standby/remove)
	State.EntityType  = MapEntityType(Pdu.EntityType);
	State.Latitude    = Lat;
	State.Longitude   = Lon;
	State.Altitude    = static_cast<float>(Alt);

	// DIS orientation: radians (psi/theta/phi, NED body-to-world) → degrees
	State.Yaw   = static_cast<float>(Pdu.Psi   * RAD_TO_DEG);
	State.Pitch = static_cast<float>(Pdu.Theta * RAD_TO_DEG);
	State.Roll  = static_cast<float>(Pdu.Phi   * RAD_TO_DEG);

	// Normalize yaw to [0, 360)
	while (State.Yaw < 0.0f)   State.Yaw += 360.0f;
	while (State.Yaw >= 360.0f) State.Yaw -= 360.0f;

	// Copy DIS entity classification for particle effects etc.
	State.EntityKind     = Pdu.EntityType.EntityKind;
	State.EntityDomain   = Pdu.EntityType.Domain;
	State.EntityCategory = Pdu.EntityType.Category;

	PendingEntityStates.Add(State);

	// Build FCigiRateControl from DIS dead reckoning params
	// DR algorithms 2 (FPW) and 5 (FPB) use linear velocity
	if (Pdu.DeadReckoning.Algorithm == 2 || Pdu.DeadReckoning.Algorithm == 5)
	{
		FCigiRateControl Rate;
		Rate.EntityId        = CamSimId;
		Rate.ArtPartId       = 0;
		Rate.bApplyToArtPart = false;

		// DIS linear velocity is in entity body frame (m/s)
		Rate.XRate = Pdu.DeadReckoning.VelX;
		Rate.YRate = Pdu.DeadReckoning.VelY;
		Rate.ZRate = Pdu.DeadReckoning.VelZ;

		// DIS angular velocity is rad/s → convert to deg/s
		// DR algo 5 includes angular velocity; algo 2 does not
		if (Pdu.DeadReckoning.Algorithm == 5)
		{
			Rate.RollRate  = static_cast<float>(Pdu.DeadReckoning.AngVelX * RAD_TO_DEG);
			Rate.PitchRate = static_cast<float>(Pdu.DeadReckoning.AngVelY * RAD_TO_DEG);
			Rate.YawRate   = static_cast<float>(Pdu.DeadReckoning.AngVelZ * RAD_TO_DEG);
		}

		PendingRateControls.Add(Rate);
	}
}

// -------------------------------------------------------------------------
// SweepTimeouts — remove entities that haven't sent an update
// -------------------------------------------------------------------------

void FDisEntityAdapter::SweepTimeouts()
{
	const double NowSec = FPlatformTime::Seconds();
	const double TimeoutSec = static_cast<double>(Config.DIS.HeartbeatTimeoutSec);

	TArray<FDisEntityId> TimedOut;

	for (const auto& Pair : EntityTimestamps)
	{
		if ((NowSec - Pair.Value.LastUpdateSec) >= TimeoutSec)
		{
			TimedOut.Add(Pair.Key);
		}
	}

	for (const FDisEntityId& DisId : TimedOut)
	{
		const uint16* CamSimIdPtr = DisIdMap.Find(DisId);
		if (CamSimIdPtr)
		{
			// Emit a Remove state
			FCigiEntityState RemoveState;
			RemoveState.EntityId    = *CamSimIdPtr;
			RemoveState.EntityState = 2;  // Remove
			PendingEntityStates.Add(RemoveState);

			UE_LOG(LogCamSim, Log, TEXT("FDisEntityAdapter: entity %s timed out → remove (id=%u)"),
				*DisId.ToString(), *CamSimIdPtr);

			ReleaseId(DisId);
		}
		EntityTimestamps.Remove(DisId);
	}
}

// -------------------------------------------------------------------------
// ECEF → Geodetic conversion (Bowring iterative method)
// -------------------------------------------------------------------------

void FDisEntityAdapter::EcefToGeodetic(double X, double Y, double Z,
                                       double& OutLat, double& OutLon, double& OutAlt)
{
	// Longitude is straightforward
	OutLon = FMath::Atan2(Y, X) * RAD_TO_DEG;

	// Distance from Z-axis
	const double P = FMath::Sqrt(X * X + Y * Y);

	// Initial approximation using spherical latitude
	double Lat = FMath::Atan2(Z, P * (1.0 - WGS84_E2));

	// Iterative Bowring method (converges in 2-3 iterations for <1mm accuracy)
	for (int32 i = 0; i < 5; ++i)
	{
		const double SinLat = FMath::Sin(Lat);
		const double CosLat = FMath::Cos(Lat);
		const double N = WGS84_A / FMath::Sqrt(1.0 - WGS84_E2 * SinLat * SinLat);
		Lat = FMath::Atan2(Z + WGS84_E2 * N * SinLat, P);
	}

	// Compute altitude
	const double SinLat = FMath::Sin(Lat);
	const double CosLat = FMath::Cos(Lat);
	const double N = WGS84_A / FMath::Sqrt(1.0 - WGS84_E2 * SinLat * SinLat);

	if (FMath::Abs(CosLat) > 1e-10)
	{
		OutAlt = P / CosLat - N;
	}
	else
	{
		OutAlt = FMath::Abs(Z) - WGS84_B;
	}

	OutLat = Lat * RAD_TO_DEG;
}

// -------------------------------------------------------------------------
// ID Translation
// -------------------------------------------------------------------------

uint16 FDisEntityAdapter::GetOrAllocateId(const FDisEntityId& DisId)
{
	if (const uint16* Existing = DisIdMap.Find(DisId))
	{
		return *Existing;
	}

	uint16 NewId;
	if (FreeIds.Num() > 0)
	{
		NewId = FreeIds.Pop();
	}
	else
	{
		const uint16 MaxId = static_cast<uint16>(Config.DIS.IdBaseOffset + 4096);
		if (NextId >= MaxId)
		{
			UE_LOG(LogCamSim, Error,
				TEXT("FDisEntityAdapter: DIS entity ID pool exhausted (%d active entities)"),
				DisIdMap.Num());
			// Return a sentinel — the entity will get a duplicate ID but this is a
			// hard limit rather than silently corrupting the entity map.
			return MaxId;
		}
		NewId = NextId++;
	}

	DisIdMap.Add(DisId, NewId);
	return NewId;
}

void FDisEntityAdapter::ReleaseId(const FDisEntityId& DisId)
{
	uint16 RemovedId;
	if (DisIdMap.RemoveAndCopyValue(DisId, RemovedId))
	{
		FreeIds.Add(RemovedId);
	}
}

// -------------------------------------------------------------------------
// Entity Type Mapping
// -------------------------------------------------------------------------

uint16 FDisEntityAdapter::MapEntityType(const FDisEntityType& DisType) const
{
	// 1. Exact match
	const FString ExactKey = DisType.ToString();
	if (const uint16* Found = ExactTypeMap.Find(ExactKey))
	{
		return *Found;
	}

	// 2. Fuzzy match (kind:domain:category only)
	const FString FuzzyKey = FString::Printf(TEXT("%u:%u:%u"),
		DisType.EntityKind, DisType.Domain, DisType.Category);
	if (const uint16* Found = FuzzyTypeMap.Find(FuzzyKey))
	{
		return *Found;
	}

	// 3. Default
	return DefaultEntityTypeId;
}

void FDisEntityAdapter::BuildTypeMaps()
{
	DefaultEntityTypeId = static_cast<uint16>(Config.DIS.DefaultEntityTypeId);

	for (const auto& Mapping : Config.DIS.EntityTypeMappings)
	{
		ExactTypeMap.Add(Mapping.Key, Mapping.Value);

		// Also add a fuzzy key (kind:domain:category) if not already present
		// Key format: "kind:domain:country:category:subcategory:specific:extra"
		// Fuzzy uses indices 0, 1, 3 (kind, domain, category — skipping country)
		TArray<FString> Parts;
		Mapping.Key.ParseIntoArray(Parts, TEXT(":"));
		if (Parts.Num() >= 4)
		{
			const FString FuzzyKey = FString::Printf(TEXT("%s:%s:%s"),
				*Parts[0], *Parts[1], *Parts[3]);
			if (!FuzzyTypeMap.Contains(FuzzyKey))
			{
				FuzzyTypeMap.Add(FuzzyKey, Mapping.Value);
			}
		}
	}

	UE_LOG(LogCamSim, Log,
		TEXT("FDisEntityAdapter: type maps loaded (%d exact, %d fuzzy, default=%u)"),
		ExactTypeMap.Num(), FuzzyTypeMap.Num(), DefaultEntityTypeId);
}

// -------------------------------------------------------------------------
// Designator spot tracking (Phase 21F.2)
// -------------------------------------------------------------------------

bool FDisEntityAdapter::GetDesignatorSpot(double& OutLat, double& OutLon, double& OutAlt, int32& OutCode) const
{
	if (!bDesignatorActive) return false;
	OutLat  = DesignatorLat;
	OutLon  = DesignatorLon;
	OutAlt  = DesignatorAlt;
	OutCode = DesignatorCode;
	return true;
}

void FDisEntityAdapter::DrainDesignatorPdus()
{
	if (!Receiver) return;

	const double NowSec = FPlatformTime::Seconds();

	FDisDesignatorPdu Pdu;
	while (Receiver->DequeueDesignatorPdu(Pdu))
	{
		// Convert ECEF spot → geodetic
		EcefToGeodetic(Pdu.SpotLocationX, Pdu.SpotLocationY, Pdu.SpotLocationZ,
			DesignatorLat, DesignatorLon, DesignatorAlt);
		DesignatorCode = static_cast<int32>(Pdu.DesignatorCode);
		DesignatorUpdateTimeSec = NowSec;
		bDesignatorActive = true;
	}

	// Timeout: clear after 5s with no update
	if (bDesignatorActive && (NowSec - DesignatorUpdateTimeSec) > 5.0)
	{
		bDesignatorActive = false;
	}
}
