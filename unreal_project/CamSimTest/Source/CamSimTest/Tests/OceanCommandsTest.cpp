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
