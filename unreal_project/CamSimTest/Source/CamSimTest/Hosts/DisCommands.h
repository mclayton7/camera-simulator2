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

	/** The motion model for a dead-reckoning algorithm (unset for 0/1 = none/static). */
	TOptional<FMotionModel> ToMotionModel(const FDisEntityStatePdu& Pdu, double LatDeg, double LonDeg);
}
