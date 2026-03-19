// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Config/CamSimConfig.h"

/**
 * FScenarioRandomizer
 *
 * Static helper that applies stochastic variation to a FCamSimConfig copy
 * before FScenarioEngine::Initialize(). Mutates the config in-place.
 *
 * Seed resolution: 0 = wall-clock based (non-deterministic),
 * positive integer = deterministic. The resolved seed is stored in
 * Cfg.Randomization.ResolvedSeed for logging and reproducibility.
 */
class FScenarioRandomizer
{
public:
	static void Randomize(FCamSimConfig& Cfg);

private:
	static void RandomizeEnvironment(FCamSimConfig& Cfg, FRandomStream& Rng);
	static void RandomizeEntities(FCamSimConfig& Cfg, FRandomStream& Rng);
};
