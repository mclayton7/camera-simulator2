// Copyright CamSim Contributors. All Rights Reserved.

#include "DIS/DisEntityAdapter.h"
#include "DIS/DisReceiver.h"
#include "Config/CamSimConfig.h"
#include "CamSimTest.h"
#include "Geospatial/EcefFrames.h"
#include "Hosts/DisCommands.h"
#include "Sim/CommandSink.h"


// -------------------------------------------------------------------------
// Constructor
// -------------------------------------------------------------------------

FDisEntityAdapter::FDisEntityAdapter(const FCamSimConfig& InConfig, FDisReceiver* InReceiver)
	: Config(InConfig)
	, Receiver(InReceiver)
{
	BuildTypeMaps();
}

// -------------------------------------------------------------------------
// Tick — drain DIS PDUs, convert, sweep timeouts
// -------------------------------------------------------------------------

void FDisEntityAdapter::Poll(ISimCommandSink& Sink)
{
	if (!Receiver) return;

	FDisEntityStatePdu Pdu;
	while (Receiver->DequeueEntityStatePdu(Pdu))
	{
		ProcessPdu(Pdu, Sink);
	}
	SweepTimeouts(Sink);
	DrainDesignatorPdus();  // Phase 21F.2
}

// -------------------------------------------------------------------------
// ProcessPdu — one DIS Entity State PDU → entity command
// -------------------------------------------------------------------------

void FDisEntityAdapter::ProcessPdu(const FDisEntityStatePdu& Pdu, ISimCommandSink& Sink)
{
	FEntityTimestamp& Ts = EntityTimestamps.FindOrAdd(Pdu.EntityId);
	Ts.DisId = Pdu.EntityId;
	Ts.LastUpdateSec = FPlatformTime::Seconds();

	Sink.Submit(CamSim::Dis::ToEntityCommand(Pdu, MapEntityType(Pdu.EntityType)));
}

// -------------------------------------------------------------------------
// SweepTimeouts — remove entities that haven't sent an update
// -------------------------------------------------------------------------

void FDisEntityAdapter::SweepTimeouts(ISimCommandSink& Sink)
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
		FEntityCommand Remove;
		Remove.Key = CamSim::Dis::Key(DisId);
		Remove.Lifecycle = EEntityLifecycle::Remove;
		Sink.Submit(Remove);
		UE_LOG(LogCamSim, Log, TEXT("FDisEntityAdapter: entity %s timed out → remove"), *DisId.ToString());
		EntityTimestamps.Remove(DisId);
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

		// Also add a fuzzy key (kind:domain:category).
		// Key format: "kind:domain:country:category:subcategory:specific:extra"
		// Fuzzy uses indices 0, 1, 3 (kind, domain, category — skipping country).
		TArray<FString> Parts;
		Mapping.Key.ParseIntoArray(Parts, TEXT(":"));
		if (Parts.Num() >= 5)
		{
			const FString FuzzyKey = FString::Printf(TEXT("%s:%s:%s"),
				*Parts[0], *Parts[1], *Parts[3]);

			// Multiple mappings can produce the same fuzzy key (e.g. one specific
			// to a country/subcategory and one with country=0:sub=0 covering the
			// whole class). Prefer the most generic mapping — country=0 AND
			// subcategory=0 — since that's the one whose intent is "this is the
			// fallback for kind:domain:category". TMap iteration order is
			// unspecified, so a "first wins" policy would be non-deterministic.
			const bool bIsGeneric = (Parts[2] == TEXT("0") && Parts[4] == TEXT("0"));
			if (!FuzzyTypeMap.Contains(FuzzyKey) || bIsGeneric)
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
		CamSimFrames::EcefToGeodetic(FVector(Pdu.SpotLocationX, Pdu.SpotLocationY, Pdu.SpotLocationZ),
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
