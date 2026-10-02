// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Camera/CamSimTelemetryAssembler.h"
#include "Config/CamSimConfig.h"
#include "Geospatial/CigiFrames.h"

// -------------------------------------------------------------------------
// FCamSimTelemetryAssembler: flat-earth frame centre when the boresight
// hits no terrain.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraFlatEarthFrameCenterTest,
	"CamSim.Camera.Telemetry.FlatEarthFrameCenter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraFlatEarthFrameCenterTest::RunTest(const FString& Parameters)
{
	double Range = -1.0, Lat = 0.0, Lon = 0.0;

	// 45 deg down, looking north from 1000 m: 1414 m slant, 1000 m north.
	TestTrue(TEXT("below the horizon"),
		FCamSimTelemetryAssembler::FlatEarthFrameCenter(10.0, 20.0, 1000.0, -45.0f, 0.0f, Range, Lat, Lon));
	TestTrue(FString::Printf(TEXT("slant range %.1f"), Range), FMath::IsNearlyEqual(Range, 1414.21, 0.01));
	TestTrue(TEXT("frame centre 1000 m north"),
		CamSimFrames::GeodeticDeltaToNeu(10.0, 20.0, 1000.0, Lat, Lon, 1000.0).Equals(FVector(1000.0, 0.0, 0.0), 0.01));

	// Looking east
	FCamSimTelemetryAssembler::FlatEarthFrameCenter(0.0, 0.0, 1000.0, -45.0f, 90.0f, Range, Lat, Lon);
	TestTrue(TEXT("frame centre 1000 m east"),
		CamSimFrames::GeodeticDeltaToNeu(0.0, 0.0, 1000.0, Lat, Lon, 1000.0).Equals(FVector(0.0, 1000.0, 0.0), 0.01));

	TestFalse(TEXT("level boresight never meets flat ground"),
		FCamSimTelemetryAssembler::FlatEarthFrameCenter(0.0, 0.0, 1000.0, 0.0f, 0.0f, Range, Lat, Lon));
	TestEqual(TEXT("no slant range above the horizon"), Range, 0.0);
	return true;
}
