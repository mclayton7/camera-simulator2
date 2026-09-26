// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Geospatial/TerrainReadinessGate.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainReadinessGateTest,
	"CamSim.TerrainReadinessGate.HoldsUntilLoaded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTerrainReadinessGateTest::RunTest(const FString& Parameters)
{
	FTerrainReadinessGate Gate;
	FTerrainReadinessGate::FSettings S;
	S.MinLoadProgressPct = 99.0f;
	S.TimeoutSec         = 10.0f;
	S.TeleportDistanceM  = 5000.0;
	Gate.Configure(S);

	TestFalse(TEXT("Closed while loading at startup"), Gate.Update(0.0, 12.0f, 0.0));
	TestFalse(TEXT("Still closed at 98%"), Gate.Update(1.0, 98.0f, 0.0));
	TestTrue(TEXT("Opens at 99%"), Gate.Update(2.0, 99.0f, 0.0));

	// Normal flight: background streaming must not stall video.
	TestTrue(TEXT("Stays open while streaming during flight"), Gate.Update(3.0, 40.0f, 30.0));

	// Teleport re-arms the gate.
	TestFalse(TEXT("Teleport closes the gate"), Gate.Update(4.0, 5.0f, 250000.0));
	TestFalse(TEXT("Waits for the new area"), Gate.Update(5.0, 60.0f, 0.0));
	TestTrue(TEXT("Opens once loaded"), Gate.Update(6.0, 100.0f, 0.0));

	// Timeout opens a gate that never reaches the threshold.
	FTerrainReadinessGate Stuck;
	Stuck.Configure(S);
	TestFalse(TEXT("Closed before timeout"), Stuck.Update(100.0, 50.0f, 0.0));
	TestFalse(TEXT("Closed at 9.9 s"), Stuck.Update(109.9, 50.0f, 0.0));
	TestTrue(TEXT("Opens at timeout"), Stuck.Update(110.0, 50.0f, 0.0));
	TestTrue(TEXT("Reports the timeout"), Stuck.DidTimeOut());

	// No tilesets (e.g. flat test level) or disabled: never blocks.
	FTerrainReadinessGate None;
	None.Configure(S);
	TestTrue(TEXT("No tilesets -> ready"), None.Update(0.0, -1.0f, 0.0));
	FTerrainReadinessGate Off;
	S.bEnabled = false;
	Off.Configure(S);
	TestTrue(TEXT("Disabled -> ready"), Off.Update(0.0, 0.0f, 0.0));
	return true;
}
