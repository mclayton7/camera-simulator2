// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Scenario/ScenarioRandomizer.h"
#include "Config/CamSimConfig.h"

// ---------------------------------------------------------------------------
// Test 1: DeterministicSeed — same seed produces identical results
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeterministicSeedTest,
	"CamSim.Phase23.Randomizer.DeterministicSeed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeterministicSeedTest::RunTest(const FString& Parameters)
{
	auto BuildConfig = []() -> FCamSimConfig
	{
		FCamSimConfig Cfg;
		Cfg.bScenarioEnabled = true;

		FCamSimConfig::FScenarioEntityConfig Template;
		Template.EntityId = 2001;
		Template.EntityType = 1001;
		Template.SpawnTimeSec = 0.0f;
		Template.StartLatitude = 38.9;
		Template.StartLongitude = -77.0;
		Template.StartAltitude = 500.0;
		Cfg.ScenarioEntities.Add(Template);

		Cfg.Randomization.bEnabled = true;
		Cfg.Randomization.Seed = 42;

		FCamSimConfig::FEntityRandomizationEntry Entry;
		Entry.TemplateEntityId = 2001;
		Entry.MinCount = 2;
		Entry.MaxCount = 5;
		Entry.SpawnRadiusM = 500.0f;
		Entry.IdOffset = 100;
		Cfg.Randomization.EntityEntries.Add(Entry);

		return Cfg;
	};

	FCamSimConfig CfgA = BuildConfig();
	FCamSimConfig CfgB = BuildConfig();

	FScenarioRandomizer::Randomize(CfgA);
	FScenarioRandomizer::Randomize(CfgB);

	TestEqual(TEXT("Same entity count from same seed"),
		CfgA.ScenarioEntities.Num(), CfgB.ScenarioEntities.Num());

	// Verify all entity IDs match
	for (int32 i = 0; i < FMath::Min(CfgA.ScenarioEntities.Num(), CfgB.ScenarioEntities.Num()); ++i)
	{
		TestEqual(
			*FString::Printf(TEXT("Entity %d ID matches"), i),
			CfgA.ScenarioEntities[i].EntityId,
			CfgB.ScenarioEntities[i].EntityId);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Test 2: WallClockSeedNonZero — seed=0 resolves to non-zero
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWallClockSeedNonZeroTest,
	"CamSim.Phase23.Randomizer.WallClockSeedNonZero",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWallClockSeedNonZeroTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.bScenarioEnabled = true;

	FCamSimConfig::FScenarioEntityConfig Template;
	Template.EntityId = 1;
	Template.EntityType = 1001;
	Cfg.ScenarioEntities.Add(Template);

	Cfg.Randomization.bEnabled = true;
	Cfg.Randomization.Seed = 0; // wall-clock

	FScenarioRandomizer::Randomize(Cfg);

	TestTrue(TEXT("ResolvedSeed is non-zero after wall-clock resolution"),
		Cfg.Randomization.ResolvedSeed != 0);

	return true;
}

// ---------------------------------------------------------------------------
// Test 3: EntityCountInRange — entity count always in [MinCount, MaxCount]
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityCountInRangeTest,
	"CamSim.Phase23.Randomizer.EntityCountInRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEntityCountInRangeTest::RunTest(const FString& Parameters)
{
	for (int32 Trial = 0; Trial < 50; ++Trial)
	{
		FCamSimConfig Cfg;
		Cfg.bScenarioEnabled = true;

		FCamSimConfig::FScenarioEntityConfig Template;
		Template.EntityId = 2001;
		Template.EntityType = 1001;
		Template.StartLatitude = 38.9;
		Template.StartLongitude = -77.0;
		Cfg.ScenarioEntities.Add(Template);

		Cfg.Randomization.bEnabled = true;
		Cfg.Randomization.Seed = 1000 + Trial; // different seed each trial

		FCamSimConfig::FEntityRandomizationEntry Entry;
		Entry.TemplateEntityId = 2001;
		Entry.MinCount = 2;
		Entry.MaxCount = 5;
		Entry.SpawnRadiusM = 100.0f;
		Entry.IdOffset = 100;
		Cfg.Randomization.EntityEntries.Add(Entry);

		FScenarioRandomizer::Randomize(Cfg);

		// Total entities = 1 template + (Count-1) extras, where Count in [2,5]
		// So ScenarioEntities.Num() should be in [2, 5]
		const int32 Num = Cfg.ScenarioEntities.Num();
		TestTrue(
			*FString::Printf(TEXT("Trial %d: count %d >= 2"), Trial, Num),
			Num >= 2);
		TestTrue(
			*FString::Printf(TEXT("Trial %d: count %d <= 5"), Trial, Num),
			Num <= 5);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Test 4: ExtraEntityIdOffset — cloned entity IDs follow IdOffset pattern
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExtraEntityIdOffsetTest,
	"CamSim.Phase23.Randomizer.ExtraEntityIdOffset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FExtraEntityIdOffsetTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.bScenarioEnabled = true;

	FCamSimConfig::FScenarioEntityConfig Template;
	Template.EntityId = 2001;
	Template.EntityType = 1001;
	Template.StartLatitude = 38.9;
	Template.StartLongitude = -77.0;
	Cfg.ScenarioEntities.Add(Template);

	Cfg.Randomization.bEnabled = true;
	Cfg.Randomization.Seed = 99;

	FCamSimConfig::FEntityRandomizationEntry Entry;
	Entry.TemplateEntityId = 2001;
	Entry.MinCount = 3;
	Entry.MaxCount = 3; // exactly 3 entities total
	Entry.SpawnRadiusM = 100.0f;
	Entry.IdOffset = 100;
	Cfg.Randomization.EntityEntries.Add(Entry);

	FScenarioRandomizer::Randomize(Cfg);

	// Should have 3 entities: template (2001) + extras (2101, 2201)
	TestEqual(TEXT("Total entity count is 3"), Cfg.ScenarioEntities.Num(), 3);

	bool bFound2101 = false;
	bool bFound2201 = false;
	for (const FCamSimConfig::FScenarioEntityConfig& Spec : Cfg.ScenarioEntities)
	{
		if (Spec.EntityId == 2101) bFound2101 = true;
		if (Spec.EntityId == 2201) bFound2201 = true;
	}

	TestTrue(TEXT("Entity ID 2101 exists (offset 100 * 1)"), bFound2101);
	TestTrue(TEXT("Entity ID 2201 exists (offset 100 * 2)"), bFound2201);

	return true;
}

// ---------------------------------------------------------------------------
// Test 5: PositionJitter — template position differs from original
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPositionJitterTest,
	"CamSim.Phase23.Randomizer.PositionJitter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPositionJitterTest::RunTest(const FString& Parameters)
{
	const double OrigLat = 38.9;
	const double OrigLon = -77.0;

	FCamSimConfig Cfg;
	Cfg.bScenarioEnabled = true;

	FCamSimConfig::FScenarioEntityConfig Template;
	Template.EntityId = 2001;
	Template.EntityType = 1001;
	Template.StartLatitude = OrigLat;
	Template.StartLongitude = OrigLon;
	Cfg.ScenarioEntities.Add(Template);

	Cfg.Randomization.bEnabled = true;
	Cfg.Randomization.Seed = 77;

	FCamSimConfig::FEntityRandomizationEntry Entry;
	Entry.TemplateEntityId = 2001;
	Entry.MinCount = 1;
	Entry.MaxCount = 1; // no clones, just template jitter
	Entry.SpawnRadiusM = 0.0f;
	Entry.PositionJitterM = 100.0f;
	Entry.IdOffset = 100;
	Cfg.Randomization.EntityEntries.Add(Entry);

	FScenarioRandomizer::Randomize(Cfg);

	TestEqual(TEXT("Still 1 entity (no clones)"), Cfg.ScenarioEntities.Num(), 1);

	const double NewLat = Cfg.ScenarioEntities[0].StartLatitude;
	const double NewLon = Cfg.ScenarioEntities[0].StartLongitude;

	// Position should have changed
	const bool bLatChanged = FMath::Abs(NewLat - OrigLat) > 1e-9;
	const bool bLonChanged = FMath::Abs(NewLon - OrigLon) > 1e-9;
	TestTrue(TEXT("Template position was jittered"), bLatChanged || bLonChanged);

	// Jitter should be bounded: 100m ~= 0.0009 degrees lat
	const double MaxJitterDeg = 100.0 / 111320.0 + 0.0001; // small tolerance
	TestTrue(TEXT("Latitude jitter within 100m"),
		FMath::Abs(NewLat - OrigLat) <= MaxJitterDeg);
	TestTrue(TEXT("Longitude jitter within ~100m"),
		FMath::Abs(NewLon - OrigLon) <= 0.002); // wider for longitude at this latitude

	return true;
}

// ---------------------------------------------------------------------------
// Test 6: EnvironmentHourClamped — ScenarioStartHour always in [0, 24)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentHourClampedTest,
	"CamSim.Phase23.Randomizer.EnvironmentHourClamped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEnvironmentHourClampedTest::RunTest(const FString& Parameters)
{
	for (int32 Trial = 0; Trial < 100; ++Trial)
	{
		FCamSimConfig Cfg;
		Cfg.bScenarioEnabled = true;
		Cfg.ScenarioStartHour = 23.0f;

		FCamSimConfig::FScenarioEntityConfig Template;
		Template.EntityId = 1;
		Template.EntityType = 1001;
		Cfg.ScenarioEntities.Add(Template);

		Cfg.Randomization.bEnabled = true;
		Cfg.Randomization.Seed = 500 + Trial;
		Cfg.Randomization.StartHourJitterHrs = 5.0f;

		FScenarioRandomizer::Randomize(Cfg);

		TestTrue(
			*FString::Printf(TEXT("Trial %d: hour %.2f >= 0"), Trial, Cfg.ScenarioStartHour),
			Cfg.ScenarioStartHour >= 0.0f);
		TestTrue(
			*FString::Printf(TEXT("Trial %d: hour %.2f < 24"), Trial, Cfg.ScenarioStartHour),
			Cfg.ScenarioStartHour < 24.0f);
	}

	return true;
}
