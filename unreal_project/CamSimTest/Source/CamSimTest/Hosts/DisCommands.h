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
	 * Entity State PDU → entity update with its dead-reckoning motion model.
	 * TypeId is the CamSim entity type the adapter mapped the DIS type to.
	 */
	FEntityCommand ToEntityCommand(const FDisEntityStatePdu& Pdu, uint16 TypeId);

	/** The motion model for a dead-reckoning algorithm (unset for 0/1 = none/static). */
	TOptional<FMotionModel> ToMotionModel(const FDisEntityStatePdu& Pdu, double LatDeg, double LonDeg);
}
