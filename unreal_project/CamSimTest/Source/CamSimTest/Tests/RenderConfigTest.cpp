// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

// -------------------------------------------------------------------------
// ROADMAP 3A: render path, snapshot endpoint and frame-stats settings reach
// FCamSimConfig from YAML and from env vars; a bad view_source falls back.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderConfigSectionTest,
	"CamSim.Config.RenderSection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderConfigSectionTest::RunTest(const FString& Parameters)
{
	using EViewSource = FCamSimConfig::FRenderConfig::EViewSource;

	const FCamSimConfig D;
	TestTrue(TEXT("default view source is primary"), D.Render.IsPrimary());
	TestEqual(TEXT("origin shift off by default"), D.Render.OriginShiftDistanceM, 0.0);
	TestFalse(TEXT("snapshot endpoint off by default"), D.Operational.bSnapshotEndpointEnabled);
	TestTrue(TEXT("frame stats off by default"), D.Operational.FrameStatsPath.IsEmpty());

	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
		"render:\n"
		"  view_source: scene_capture\n"
		"  camera_cut_distance_m: 250.0\n"
		"  camera_cut_angle_deg: 15.0\n"
		"  origin_shift_distance_m: 20000.0\n"
		"operational:\n"
		"  snapshot_endpoint_enabled: true\n"
		"  frame_stats_path: \"/tmp/camsim-frames.jsonl\"\n"));
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	TestTrue(TEXT("scene_capture parsed"), Cfg.Render.ViewSourceMode == EViewSource::SceneCapture);
	TestFalse(TEXT("IsPrimary false"), Cfg.Render.IsPrimary());
	TestEqual(TEXT("cut distance"), Cfg.Render.CameraCutDistanceM, 250.0f);
	TestEqual(TEXT("cut angle"), Cfg.Render.CameraCutAngleDeg, 15.0f);
	TestEqual(TEXT("origin shift distance"), Cfg.Render.OriginShiftDistanceM, 20000.0);
	TestTrue(TEXT("snapshot enabled"), Cfg.Operational.bSnapshotEndpointEnabled);
	TestEqual(TEXT("frame stats path"), Cfg.Operational.FrameStatsPath, FString(TEXT("/tmp/camsim-frames.jsonl")));

	AddExpectedMessage(TEXT("Unknown render.view_source 'primery'"), EAutomationExpectedErrorFlags::Contains, 1);
	const FCamSimConfig Bad = FCamSimConfig::LoadFromYamlString(TEXT("render:\n  view_source: primery\n"));
	TestTrue(TEXT("typo falls back to primary"), Bad.Render.IsPrimary());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderConfigEnvTest,
	"CamSim.Config.RenderEnvOverrides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderConfigEnvTest::RunTest(const FString& Parameters)
{
	struct FEnv { const TCHAR* Key; const TCHAR* Value; };
	const FEnv Vars[] = {
		{ TEXT("CAMSIM_RENDER_VIEW_SOURCE"),             TEXT("scene_capture") },
		{ TEXT("CAMSIM_RENDER_CAMERA_CUT_DISTANCE_M"),   TEXT("123") },
		{ TEXT("CAMSIM_RENDER_CAMERA_CUT_ANGLE_DEG"),    TEXT("12") },
		{ TEXT("CAMSIM_RENDER_ORIGIN_SHIFT_DISTANCE_M"), TEXT("5000") },
		{ TEXT("CAMSIM_SNAPSHOT_ENDPOINT_ENABLED"),      TEXT("1") },
		{ TEXT("CAMSIM_FRAME_STATS_PATH"),               TEXT("/tmp/x.jsonl") },
	};
	for (const FEnv& V : Vars) { FPlatformMisc::SetEnvironmentVar(V.Key, V.Value); }

	// The loader applies env overrides after YAML (ApplyEnvOverrides is private).
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT("cigi_port: 8888\n"));

	for (const FEnv& V : Vars) { FPlatformMisc::SetEnvironmentVar(V.Key, TEXT("")); }

	TestFalse(TEXT("env view source"), Cfg.Render.IsPrimary());
	TestEqual(TEXT("env cut distance"), Cfg.Render.CameraCutDistanceM, 123.0f);
	TestEqual(TEXT("env cut angle"), Cfg.Render.CameraCutAngleDeg, 12.0f);
	TestEqual(TEXT("env origin shift"), Cfg.Render.OriginShiftDistanceM, 5000.0);
	TestTrue(TEXT("env snapshot"), Cfg.Operational.bSnapshotEndpointEnabled);
	TestEqual(TEXT("env frame stats"), Cfg.Operational.FrameStatsPath, FString(TEXT("/tmp/x.jsonl")));
	return true;
}
