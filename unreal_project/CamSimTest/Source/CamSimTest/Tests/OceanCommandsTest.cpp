// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Ocean/OceanCommands.h"
#include "Sim/Commands.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanCommandsWaveTest, "CamSim.Ocean.Commands.WaveCommandToWave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanCommandsWaveTest::RunTest(const FString& Parameters)
{
	FOceanWaveCommand C;
	C.WaveId = 1; C.bEnabled = true; C.HeightM = 2.0f; C.LengthM = 50.0f; C.PeriodS = 0.0f;
	C.DirectionDeg = 90.0f; C.PhaseOffsetDeg = 180.0f;
	const TOptional<FOceanWave> W = CamSimOcean::ToOceanWave(C);
	TestTrue (TEXT("enabled → wave"), W.IsSet());
	TestEqual(TEXT("propagates toward 90 → from 270"), W->FromDeg, 270.0, 1e-9);
	TestEqual(TEXT("phase in radians"), W->PhaseRad, UE_DOUBLE_PI, 1e-9);
	TestEqual(TEXT("height"), W->HeightM, 2.0, 1e-9);

	C.bEnabled = false;
	TestFalse(TEXT("disabled → removal"), CamSimOcean::ToOceanWave(C).IsSet());
	C.bEnabled = true; C.HeightM = NAN;
	TestFalse(TEXT("non-finite → removal"), CamSimOcean::ToOceanWave(C).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanCommandsPeriodOnlyTest, "CamSim.Ocean.Commands.PeriodOnlyWaveLength",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanCommandsPeriodOnlyTest::RunTest(const FString& Parameters)
{
	FOceanWaveCommand C;
	C.bEnabled = true; C.HeightM = 1.0f; C.LengthM = 0.0f; C.PeriodS = 8.0f;
	TOptional<FOceanWave> W = CamSimOcean::ToOceanWave(C);
	TestTrue(TEXT("period only → wave"), W.IsSet());
	// lambda = g T^2 / (2 pi) = 9.80665 * 64 / (2 pi) ≈ 99.89 m
	TestEqual(TEXT("length from deep-water dispersion"), W->LengthM, 9.80665 * 64.0 / (2.0 * UE_DOUBLE_PI), 1e-4);
	TestEqual(TEXT("period kept"), W->PeriodS, 8.0, 1e-9);

	C.LengthM = -5.0f;
	TestEqual(TEXT("negative length also derived"), CamSimOcean::ToOceanWave(C)->LengthM, 9.80665 * 64.0 / (2.0 * UE_DOUBLE_PI), 1e-4);

	C.LengthM = 30.0f;
	TestEqual(TEXT("explicit length wins"), CamSimOcean::ToOceanWave(C)->LengthM, 30.0, 1e-9);

	C.LengthM = 0.0f; C.PeriodS = 0.0f;
	TestEqual(TEXT("no length, no period: 0 (dropped by SetWaves)"), CamSimOcean::ToOceanWave(C)->LengthM, 0.0, 1e-12);
	return true;
}
