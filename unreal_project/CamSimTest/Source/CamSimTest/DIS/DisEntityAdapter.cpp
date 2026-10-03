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
	// Appearance bit 23 (deactivated): the sender deleted the entity, remove it now instead of waiting for the heartbeat
	// timeout. Later deactivated PDUs for it (or for one never seen) are ignored, so they neither spawn nor re-remove it.
	if (CamSim::Dis::IsDeactivated(Pdu.Appearance))
	{
		if (EntityTimestamps.Remove(Pdu.EntityId) > 0)
		{
			RemoveEntity(Pdu.EntityId, Sink);
			UE_LOG(LogCamSim, Log, TEXT("FDisEntityAdapter: entity %s deactivated → remove"), *Pdu.EntityId.ToString());
		}
		return;
	}

	FEntityTimestamp& Ts = EntityTimestamps.FindOrAdd(Pdu.EntityId);
	Ts.DisId = Pdu.EntityId;
	Ts.LastUpdateSec = FPlatformTime::Seconds();

	Sink.Submit(CamSim::Dis::ToEntityCommand(Pdu, MapEntityType(Pdu.EntityType), Config.DIS.bClampToSurface));

	// ROADMAP 4C: platform appearance (power plant, damage, flaming) as component commands, only on change.
	if (const TOptional<CamSim::Dis::FPlatformAppearance> A = CamSim::Dis::DecodePlatformAppearance(
		Pdu.EntityType.EntityKind, Pdu.EntityType.Domain, Pdu.Appearance))
	{
		const CamSim::Dis::FPlatformAppearance* Prev = LastAppearance.Find(Pdu.EntityId);
		TArray<FComponentCommand> Commands;
		CamSim::Dis::AppearanceCommands(CamSim::Dis::Key(Pdu.EntityId),
			Prev ? TOptional<CamSim::Dis::FPlatformAppearance>(*Prev) : TOptional<CamSim::Dis::FPlatformAppearance>(), *A, Commands);
		for (const FComponentCommand& C : Commands) Sink.Submit(C);
		LastAppearance.Add(Pdu.EntityId, *A);
	}
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
		EntityTimestamps.Remove(DisId);
		RemoveEntity(DisId, Sink);
		UE_LOG(LogCamSim, Log, TEXT("FDisEntityAdapter: entity %s timed out → remove"), *DisId.ToString());
	}
}

void FDisEntityAdapter::RemoveEntity(const FDisEntityId& DisId, ISimCommandSink& Sink)
{
	FEntityCommand Remove;
	Remove.Key = CamSim::Dis::Key(DisId);
	Remove.Lifecycle = EEntityLifecycle::Remove;
	Sink.Submit(Remove);
	LastAppearance.Remove(DisId);
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

	// 3. Domain fallback (kind:domain)
	const FString DomainKey = FString::Printf(TEXT("%u:%u"), DisType.EntityKind, DisType.Domain);
	if (const uint16* Found = DomainTypeMap.Find(DomainKey))
	{
		return *Found;
	}

	// 4. Default
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

		if (Parts.Num() >= 2)
		{
			// Domain fallback (kind:domain): any other platform of the domain. Prefer
			// a subcategory-0 mapping, then the lower type ID, so the pick is deterministic.
			const FString DomainKey = FString::Printf(TEXT("%s:%s"), *Parts[0], *Parts[1]);
			const bool bGenericSub = Parts.Num() < 5 || Parts[4] == TEXT("0");
			if (const uint16* Existing = DomainTypeMap.Find(DomainKey))
			{
				const bool bExistingGeneric = DomainGeneric.Contains(DomainKey);
				if ((bGenericSub && !bExistingGeneric) || (bGenericSub == bExistingGeneric && Mapping.Value < *Existing))
				{
					DomainTypeMap.Add(DomainKey, Mapping.Value);
					if (bGenericSub) DomainGeneric.Add(DomainKey);
				}
			}
			else
			{
				DomainTypeMap.Add(DomainKey, Mapping.Value);
				if (bGenericSub) DomainGeneric.Add(DomainKey);
			}
		}
	}

	UE_LOG(LogCamSim, Log,
		TEXT("FDisEntityAdapter: type maps loaded (%d exact, %d fuzzy, %d domain, default=%u)"),
		ExactTypeMap.Num(), FuzzyTypeMap.Num(), DomainTypeMap.Num(), DefaultEntityTypeId);
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
