// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sim/Commands.h"

struct FCigiEntityState;
struct FCigiConfClampEntityState;
struct FCigiRateControl;
struct FCigiArtPartControl;
struct FCigiComponentControl;
struct FCigiViewDefinition;
struct FCigiViewControl;
struct FCigiSensorControl;
struct FCigiCelestialState;
struct FCigiAtmosphereState;
struct FCigiWeatherState;
struct FCigiWaveState;
struct FCigiMaritimeSurfaceState;

/**
 * CIGI 3.3 → canonical commands. Pure functions: everything CIGI-specific
 * (entity state codes, attach semantics, sensor IDs as wavebands, gain as
 * zoom) is decided here, so the simulation never sees CIGI types.
 */
namespace CamSim::Cigi
{
	inline FEntityKey Key(uint16 EntityId) { return FEntityKey(EHostSource::Cigi, EntityId); }

	FEntityCommand ToEntityCommand(const FCigiEntityState& In);
	FEntityCommand ToEntityCommand(const FCigiConfClampEntityState& In);

	/** Rate Control for a whole entity; unset for articulated-part rates (not modelled). */
	TOptional<FEntityMotionCommand> ToMotionCommand(const FCigiRateControl& In);

	FArticulationCommand ToArticulationCommand(const FCigiArtPartControl& In);
	FComponentCommand ToComponentCommand(const FCigiComponentControl& In);

	FViewCommand ToViewCommand(const FCigiViewDefinition& In);
	/** View Control: snap the gimbal; a non-zero entity starts, 0 ends, the first-person view. */
	FViewCommand ToViewCommand(const FCigiViewControl& In);
	/** Articulated Part Control addressed to the camera entity: a rate-limited gimbal target. */
	FViewCommand ToGimbalSlewCommand(const FCigiArtPartControl& In);

	FSensorCommand ToSensorCommand(const FCigiSensorControl& In);

	/** Celestial Sphere Control; Date/Time Valid with an impossible date yields no Utc. */
	FTimeCommand ToTimeCommand(const FCigiCelestialState& In);

	FAtmosphereCommand ToAtmosphereCommand(const FCigiAtmosphereState& In);
	FWeatherCommand ToWeatherCommand(const FCigiWeatherState& In);
	FOceanWaveCommand ToOceanWaveCommand(const FCigiWaveState& In);
	FMaritimeSurfaceCommand ToMaritimeSurfaceCommand(const FCigiMaritimeSurfaceState& In);
}
