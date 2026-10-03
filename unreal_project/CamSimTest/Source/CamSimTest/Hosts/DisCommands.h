// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sim/Commands.h"

struct FDisEntityId;
struct FDisEntityStatePdu;

/**
 * DIS (IEEE 1278.1) → canonical commands. Pure functions: ECEF positions,
 * ECEF-referenced Euler angles and dead-reckoning frames are resolved here.
 */
namespace CamSim::Dis
{
	/** site:application:entity packed into the key's 48 low bits. */
	FEntityKey Key(const FDisEntityId& Id);

	/**
	 * DIS kind/domain → surface placement: land platforms (kind 1, domain 1) on the
	 * ground, surface platforms (kind 1, domain 3) on the water; every other kind
	 * (munitions — whose domain is the target's —, life forms, …) as sent.
	 */
	ESurfaceMode SurfaceModeFor(uint8 Kind, uint8 Domain, bool bClampToSurface);

	/**
	 * Entity State PDU → entity update with its dead-reckoning motion model.
	 * TypeId is the CamSim entity type the adapter mapped the DIS type to.
	 */
	FEntityCommand ToEntityCommand(const FDisEntityStatePdu& Pdu, uint16 TypeId, bool bClampToSurface = true);

	/**
	 * IEEE 1278.1 appearance bit 23 (State: 0 active, 1 deactivated), common to every entity kind. A simulation manager
	 * sends it when it deletes an entity.
	 */
	inline bool IsDeactivated(uint32 Appearance) { return ((Appearance >> 23) & 1u) != 0u; }

	/** Platform appearance fields CamSim uses (ROADMAP 4C). Damage: 0 none, 1 slight/moderate, 2 destroyed. */
	struct FPlatformAppearance
	{
		bool  bPowerPlant = false;
		uint8 Damage      = 0;
		bool  bFlaming    = false;
		bool operator==(const FPlatformAppearance&) const = default;
	};

	/**
	 * IEEE 1278.1 / SISO-REF-010 platform appearance (land, air and surface share these bits): bits 3-4 damage, bit 15
	 * flaming, bit 22 power plant on. Unset for other kinds/domains (munitions, life forms, subsurface, space).
	 */
	TOptional<FPlatformAppearance> DecodePlatformAppearance(uint8 Kind, uint8 Domain, uint32 Appearance);

	/**
	 * Component commands (class 0) for the fields that changed: 10 damage, 11 power plant, 12 flaming. Previous unset (an
	 * entity's first PDU): the fields that differ from the defaults (all off).
	 */
	void AppearanceCommands(const FEntityKey& Key, const TOptional<FPlatformAppearance>& Previous, const FPlatformAppearance& Now,
		TArray<FComponentCommand>& Out);

	/** The motion model for a dead-reckoning algorithm (unset for 0/1 = none/static). */
	TOptional<FMotionModel> ToMotionModel(const FDisEntityStatePdu& Pdu, double LatDeg, double LonDeg);
}
