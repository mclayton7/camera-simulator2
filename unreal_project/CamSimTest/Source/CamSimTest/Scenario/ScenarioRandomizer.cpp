// Copyright CamSim Contributors. All Rights Reserved.

#include "Scenario/ScenarioRandomizer.h"
#include "CamSimTest.h"
#include "Math/RandomStream.h"
#include "HAL/PlatformTime.h"

#include "Geospatial/GeoConstants.h"

void FScenarioRandomizer::Randomize(FCamSimConfig& Cfg)
{
	FCamSimConfig::FRandomizationConfig& R = Cfg.Randomization;
	if (!R.bEnabled) return;

	// Resolve seed
	if (R.Seed == 0)
	{
		R.ResolvedSeed = static_cast<int32>(FPlatformTime::Seconds() * 1000.0) & 0x7FFFFFFF;
	}
	else
	{
		R.ResolvedSeed = R.Seed;
	}

	UE_LOG(LogCamSim, Log, TEXT("ScenarioRandomizer: using seed %d"), R.ResolvedSeed);

	FRandomStream Rng(R.ResolvedSeed);
	RandomizeEnvironment(Cfg, Rng);
	RandomizeEntities(Cfg, Rng);
}

void FScenarioRandomizer::RandomizeEnvironment(FCamSimConfig& Cfg, FRandomStream& Rng)
{
	const FCamSimConfig::FRandomizationConfig& R = Cfg.Randomization;

	// Time-of-day jitter
	if (R.StartHourJitterHrs > 0.0f)
	{
		Cfg.ScenarioStartHour += Rng.FRandRange(-R.StartHourJitterHrs, R.StartHourJitterHrs);
		Cfg.ScenarioStartHour = FMath::Fmod(Cfg.ScenarioStartHour + 24.0f, 24.0f);
	}

	// Visibility jitter
	if (R.VisibilityJitterFrac > 0.0f)
	{
		const float Factor = 1.0f + Rng.FRandRange(-R.VisibilityJitterFrac, R.VisibilityJitterFrac);
		Cfg.Phase18.VisibilityRangeM = FMath::Max(500.0f, Cfg.Phase18.VisibilityRangeM * Factor);
	}

	// Fog density jitter
	if (R.FogDensityJitterFrac > 0.0f)
	{
		const float Factor = 1.0f + Rng.FRandRange(-R.FogDensityJitterFrac, R.FogDensityJitterFrac);
		Cfg.Phase18.FogDensity = FMath::Clamp(Cfg.Phase18.FogDensity * Factor, 0.0f, 1.0f);
	}

	// Random weather
	if (R.bRandomizeWeather)
	{
		Cfg.Phase18.bPrecipitation = (Rng.GetFraction() < R.WeatherProbability);
	}
}

void FScenarioRandomizer::RandomizeEntities(FCamSimConfig& Cfg, FRandomStream& Rng)
{
	const FCamSimConfig::FRandomizationConfig& R = Cfg.Randomization;

	// Collect new entities to append after iteration
	TArray<FCamSimConfig::FScenarioEntityConfig> NewEntities;

	for (const FCamSimConfig::FEntityRandomizationEntry& Entry : R.EntityEntries)
	{
		// Find the template entity
		FCamSimConfig::FScenarioEntityConfig* Template = nullptr;
		for (FCamSimConfig::FScenarioEntityConfig& Spec : Cfg.ScenarioEntities)
		{
			if (Spec.EntityId == Entry.TemplateEntityId)
			{
				Template = &Spec;
				break;
			}
		}
		if (!Template)
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("ScenarioRandomizer: template entity %d not found, skipping"),
				Entry.TemplateEntityId);
			continue;
		}

		// Jitter the template entity's position
		if (Entry.PositionJitterM > 0.0f)
		{
			const double LatJitterDeg = Entry.PositionJitterM / METRES_PER_DEGREE_LAT;
			const double CosLat = FMath::Max(0.001,
				FMath::Abs(FMath::Cos(FMath::DegreesToRadians(Template->StartLatitude))));
			const double LonJitterDeg = Entry.PositionJitterM / (METRES_PER_DEGREE_LAT * CosLat);

			Template->StartLatitude  += Rng.FRandRange(-LatJitterDeg, LatJitterDeg);
			Template->StartLongitude += Rng.FRandRange(-LonJitterDeg, LonJitterDeg);
		}

		// Determine entity count
		const int32 MinC = FMath::Max(1, Entry.MinCount);
		const int32 MaxC = FMath::Max(MinC, Entry.MaxCount);
		const int32 Count = Rng.RandRange(MinC, MaxC);

		// Clone extra entities (count - 1 extras, since template is count 1)
		for (int32 i = 1; i < Count; ++i)
		{
			FCamSimConfig::FScenarioEntityConfig Clone = *Template;

			// Assign unique entity ID
			const int32 NewId = Template->EntityId + Entry.IdOffset * i;
			if (NewId > 65535)
			{
				UE_LOG(LogCamSim, Warning,
					TEXT("ScenarioRandomizer: entity ID %d exceeds uint16 range, skipping"),
					NewId);
				continue;
			}
			Clone.EntityId = NewId;

			// Jitter position within spawn radius
			if (Entry.SpawnRadiusM > 0.0f)
			{
				const double LatJitterDeg = Entry.SpawnRadiusM / METRES_PER_DEGREE_LAT;
				const double CosLat = FMath::Max(0.001,
					FMath::Abs(FMath::Cos(FMath::DegreesToRadians(Template->StartLatitude))));
				const double LonJitterDeg = Entry.SpawnRadiusM / (METRES_PER_DEGREE_LAT * CosLat);

				Clone.StartLatitude  = Template->StartLatitude  + Rng.FRandRange(-LatJitterDeg, LatJitterDeg);
				Clone.StartLongitude = Template->StartLongitude + Rng.FRandRange(-LonJitterDeg, LonJitterDeg);
			}

			// Jitter speed
			if (Entry.SpeedJitterFrac > 0.0f && Clone.BaseSpeedMps > 0.0f)
			{
				const float Factor = 1.0f + Rng.FRandRange(-Entry.SpeedJitterFrac, Entry.SpeedJitterFrac);
				Clone.BaseSpeedMps = FMath::Max(0.1f, Clone.BaseSpeedMps * Factor);
			}

			NewEntities.Add(MoveTemp(Clone));
		}
	}

	Cfg.ScenarioEntities.Append(NewEntities);
}
