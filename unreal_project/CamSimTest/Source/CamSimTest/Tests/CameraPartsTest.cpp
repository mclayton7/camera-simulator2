// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Camera/CamSimTelemetryAssembler.h"
#include "Config/CamSimConfig.h"
#include "Geospatial/CigiFrames.h"

// -------------------------------------------------------------------------
// FCamSimTelemetryAssembler: the frame centre when the boresight hits no
// terrain — its intersection with the ellipsoid (raised to a surface height).
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraFlatEarthFrameCenterTest,
	"CamSim.Camera.Telemetry.FlatEarthFrameCenter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraFlatEarthFrameCenterTest::RunTest(const FString& Parameters)
{
	using FAsm = FCamSimTelemetryAssembler;
	double Range = -1.0, Lat = 0.0, Lon = 0.0;
	const FVector North45Down(1.0, 0.0, -1.0);  // NEU
	const FVector East45Down(0.0, 1.0, -1.0);

	// 45 deg down, looking north from 1000 m: 1414 m slant, 1000 m north along the
	// ground (Earth's curvature moves it by ~8 cm at this range).
	TestTrue(TEXT("below the horizon"), FAsm::IntersectEllipsoid(10.0, 20.0, 1000.0, North45Down, 0.0, Range, Lat, Lon));
	TestTrue(FString::Printf(TEXT("slant range %.2f"), Range), FMath::IsNearlyEqual(Range, 1414.21, 0.2));
	TestTrue(TEXT("frame centre 1000 m north"),
		CamSimFrames::GeodeticDeltaToNeu(10.0, 20.0, 0.0, Lat, Lon, 0.0).Equals(FVector(1000.0, 0.0, 0.0), 0.2));

	// Looking east
	FAsm::IntersectEllipsoid(0.0, 0.0, 1000.0, East45Down, 0.0, Range, Lat, Lon);
	TestTrue(TEXT("frame centre 1000 m east"),
		CamSimFrames::GeodeticDeltaToNeu(0.0, 0.0, 0.0, Lat, Lon, 0.0).Equals(FVector(0.0, 1000.0, 0.0), 0.2));

	// A raised surface (terrain at 400 m): 600 m below the sensor, so 600 m east.
	FAsm::IntersectEllipsoid(0.0, 0.0, 1000.0, East45Down, 400.0, Range, Lat, Lon);
	TestTrue(TEXT("surface at 400 m: frame centre 600 m east"),
		CamSimFrames::GeodeticDeltaToNeu(0.0, 0.0, 400.0, Lat, Lon, 400.0).Equals(FVector(0.0, 600.0, 0.0), 0.2));

	TestFalse(TEXT("level boresight never meets the ground"),
		FAsm::IntersectEllipsoid(0.0, 0.0, 1000.0, FVector(1.0, 0.0, 0.0), 0.0, Range, Lat, Lon));
	TestEqual(TEXT("no slant range above the horizon"), Range, 0.0);

	// Curvature: from 1000 m the horizon is ~1.0 deg down. 0.5 deg down misses; 2 deg down hits ~28 km out.
	const auto Down = [](double Deg) { return FVector(FMath::Cos(FMath::DegreesToRadians(Deg)), 0.0, -FMath::Sin(FMath::DegreesToRadians(Deg))); };
	TestFalse(TEXT("0.5 deg down passes above the horizon"), FAsm::IntersectEllipsoid(0.0, 0.0, 1000.0, Down(0.5), 0.0, Range, Lat, Lon));
	TestTrue(TEXT("2 deg down meets the ground"), FAsm::IntersectEllipsoid(0.0, 0.0, 1000.0, Down(2.0), 0.0, Range, Lat, Lon));
	TestTrue(FString::Printf(TEXT("2 deg down: slant %.0f m, beyond the flat-earth 28654 m as the ground curves away"), Range),
		Range > 28654.0 && Range < 40000.0);

	TestFalse(TEXT("a sensor below the surface sees no ground"),
		FAsm::IntersectEllipsoid(0.0, 0.0, 100.0, East45Down, 400.0, Range, Lat, Lon));
	return true;
}
