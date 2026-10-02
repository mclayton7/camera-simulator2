// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/EntityThermal.h"
#include "Geospatial/EcefFrames.h"
#include "Config/CamSimConfig.h"
#include "Entity/EntityTypeTable.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// CamSim.Thermal.Entity.Integration.*: geodetic poses -> ECEF speed -> model, as ACamSimEntity::StepThermal does it.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityThermalGeodeticDriveTest, "CamSim.Thermal.Entity.Integration.GeodeticDrive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntityThermalGeodeticDriveTest::RunTest(const FString& Parameters)
{
	const FEntityThermalSettings S;
	FEntityThermalPartSpec Exh;
	Exh.Kind = EEntityThermalPartKind::Exhaust;
	const TArray<FEntityThermalPartSpec> Parts = { Exh };
	FEntitySpeedTracker Speed;
	FEntityThermalState St;
	// Parked 10 s, then 10 m/s north for 60 s at 30 Hz, from 37.795 N (spawn cold: parked at the first step)
	constexpr double MPerDegLat = 111000.0;
	for (int32 I = 0; I <= 2100; ++I)
	{
		const double T = I / 30.0;
		const double Moved = FMath::Max(T - 10.0, 0.0) * 10.0;
		const FVector E = CamSimFrames::GeodeticToEcef(37.795 + Moved / MPerDegLat, -122.46, 5.0);
		FEntityThermalInputs In;
		In.SimSec = T;
		In.SpeedMps = Speed.Update(E, T);
		CamSimEntityThermal::Step(St, In, S, Parts);
		if (I == 300) TestEqual(TEXT("cold while parked"), St.PartExcessK[0], 0.0f);
	}
	TestNearlyEqual(TEXT("speed ~10"), Speed.GetSpeedMps(), 10.0f, 0.3f);
	TestTrue(*FString::Printf(TEXT("exhaust hot after a minute of driving (%.1f K)"), St.PartExcessK[0]), St.PartExcessK[0] > 150.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityThermalShippedYamlTest, "CamSim.Thermal.Entity.Integration.ShippedYaml",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntityThermalShippedYamlTest::RunTest(const FString& Parameters)
{
	// deploy/camsim_config.yaml: thermal.entity matches the code defaults; the Ural and the Mako ship their part volumes.
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../../deploy/camsim_config.yaml")));
	FString Yaml;
	if (!TestTrue(*FString::Printf(TEXT("read %s"), *Path), FFileHelper::LoadFileToString(Yaml, *Path))) return false;
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(Yaml, Path);
	TestTrue(TEXT("thermal.entity = code defaults"), Cfg.Thermal.Entity == FEntityThermalSettings());
	FEntityTypeTable Table;
	Table.LoadFromYamlString(Yaml);
	const FEntityTypeEntry* Truck = Table.FindEntry(2001);
	const FEntityTypeEntry* Boat = Table.FindEntry(3001);
	if (!TestNotNull(TEXT("truck"), Truck) || !TestNotNull(TEXT("boat"), Boat)) return false;
	TestEqual(TEXT("truck parts"), Truck->ThermalParts.Num(), 3);
	TestEqual(TEXT("boat parts"), Boat->ThermalParts.Num(), 1);
	if (Truck->ThermalParts.Num() == 3)
	{
		TestEqual(TEXT("truck: exhaust last (wins overlaps)"), Truck->ThermalParts[2].Kind, EEntityThermalPartKind::Exhaust);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityThermalMovingSpawnTest, "CamSim.Thermal.Entity.Integration.MovingSpawnStartsRunning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEntityThermalMovingSpawnTest::RunTest(const FString& Parameters)
{
	// A vehicle that appears already driving (no power-plant command) must start at the running targets, as a parked spawn
	// starts parked: ACamSimEntity::StepThermal defers the first step until the speed is measured.
	const FEntityThermalSettings S;
	FEntityThermalPartSpec Eng;
	Eng.Kind = EEntityThermalPartKind::Engine;
	const TArray<FEntityThermalPartSpec> Parts = { Eng };
	FEntitySpeedTracker Speed;
	FEntityThermalState St;
	constexpr double MPerDegLat = 111000.0;
	for (int32 I = 0; I <= 30; ++I)
	{
		const double T = I / 30.0;
		const FVector E = CamSimFrames::GeodeticToEcef(37.795 + 15.0 * T / MPerDegLat, -122.46, 5.0);
		const float V = Speed.Update(E, T);
		if (CamSimEntityThermal::ShouldDeferFirstStep(St, Speed)) continue;
		FEntityThermalInputs In;
		In.SimSec = T;
		In.SpeedMps = V;
		CamSimEntityThermal::Step(St, In, S, Parts);
	}
	TestTrue(TEXT("initialised within 1 s"), St.bInitialized);
	TestNearlyEqual(TEXT("engine at the running target from the start"), St.PartExcessK[0], 45.0f, 0.5f);
	return true;
}
