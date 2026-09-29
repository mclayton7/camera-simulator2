// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Camera/CamSimStreamingController.h"
#include "Camera/CamSimTelemetryAssembler.h"
#include "Config/CamSimConfig.h"
#include "Geospatial/CigiFrames.h"

// -------------------------------------------------------------------------
// FCamSimStreamingController: gimbal-slew prefetch boost and adaptive SSE.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraStreamingLodTest,
	"CamSim.Camera.Streaming.LevelOfDetail",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCameraStreamingLodTest::RunTest(const FString& Parameters)
{
	const FCamSimStreamingController::FTilesets NoTilesets;
	constexpr float Dt = 1.0f / 30.0f;

	FCamSimConfig Cfg;
	Cfg.MaximumScreenSpaceError = 16.0f;
	Cfg.Performance.TilePrefetchSlewThresholdDegPerSec = 30.0f;
	Cfg.Performance.TilePrefetchBoostFrames = 3;
	Cfg.Performance.bAdaptiveSSE = false;

	FCamSimStreamingController Slew;
	Slew.UpdateLevelOfDetail(Dt, 0.0f, -90.0f, 60.0f, Cfg, NoTilesets);
	TestEqual(TEXT("still gimbal: no boost"), Slew.GetPrefetchBoostFramesRemaining(), 0);
	Slew.UpdateLevelOfDetail(Dt, 3.0f, -90.0f, 60.0f, Cfg, NoTilesets);  // 90 deg/s
	TestEqual(TEXT("fast slew starts the boost window"), Slew.GetPrefetchBoostFramesRemaining(), 3);
	Slew.UpdateLevelOfDetail(Dt, 3.0f, -90.0f, 60.0f, Cfg, NoTilesets);
	Slew.UpdateLevelOfDetail(Dt, 3.0f, -90.0f, 60.0f, Cfg, NoTilesets);
	TestEqual(TEXT("boost counts down once the slew stops"), Slew.GetPrefetchBoostFramesRemaining(), 1);

	Cfg.Performance.TilePrefetchSlewThresholdDegPerSec = 0.0f;
	Cfg.Performance.bAdaptiveSSE = true;
	Cfg.Performance.OutputFrameRateHz = 30.0f;
	Cfg.Performance.AdaptiveSSEMin = 8.0f;
	Cfg.Performance.AdaptiveSSEMax = 18.0f;

	FCamSimStreamingController Adaptive;
	for (int32 i = 0; i < 5; ++i)
	{
		Adaptive.UpdateLevelOfDetail(0.1f, 0.0f, 0.0f, 60.0f, Cfg, NoTilesets);  // 3x over budget
	}
	TestEqual(TEXT("over budget: SSE rises by 1 per frame up to the max"), Adaptive.GetAdaptiveSse(), 18.0f);
	for (int32 i = 0; i < 29; ++i)
	{
		Adaptive.UpdateLevelOfDetail(0.01f, 0.0f, 0.0f, 60.0f, Cfg, NoTilesets);  // well under budget
	}
	TestEqual(TEXT("29 frames under budget: unchanged"), Adaptive.GetAdaptiveSse(), 18.0f);
	Adaptive.UpdateLevelOfDetail(0.01f, 0.0f, 0.0f, 60.0f, Cfg, NoTilesets);
	TestEqual(TEXT("30th frame under budget: sharper by 0.5"), Adaptive.GetAdaptiveSse(), 17.5f);
	return true;
}

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
