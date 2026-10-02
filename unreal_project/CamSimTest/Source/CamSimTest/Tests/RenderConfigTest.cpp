// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"
#include "Geospatial/CesiumTuning.h"

// -------------------------------------------------------------------------
// ROADMAP 3A/3B.2: render path, snapshot endpoint and frame-stats settings
// reach FCamSimConfig from YAML and from env vars. The sensor is the primary
// view only (3B.2): render.view_source is gone, so it now reports as an
// unknown key rather than selecting a legacy path.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderConfigViewSourceKeyRemovedTest,
	"CamSim.Config.ViewSourceKeyRemoved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderConfigViewSourceKeyRemovedTest::RunTest(const FString& Parameters)
{
	AddExpectedMessage(TEXT("Config: unknown key 'render.view_source'"), EAutomationExpectedErrorFlags::Contains, 1);
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT("render:\n  view_source: scene_capture\n"));
	TestTrue(TEXT("view_source reported unknown"),
		Cfg.UnknownYamlKeys.ContainsByPredicate([](const FString& K) { return K.Contains(TEXT("view_source")); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderConfigSectionTest,
	"CamSim.Config.RenderSection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderConfigSectionTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig D;
	TestEqual(TEXT("origin shift every 20 km by default"), D.Render.OriginShiftDistanceM, 20000.0);
	TestFalse(TEXT("snapshot endpoint off by default"), D.Operational.bSnapshotEndpointEnabled);
	TestTrue(TEXT("frame stats off by default"), D.Operational.FrameStatsPath.IsEmpty());

	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
		"render:\n"
		"  camera_cut_distance_m: 250.0\n"
		"  camera_cut_angle_deg: 15.0\n"
		"  origin_shift_distance_m: 20000.0\n"
		"operational:\n"
		"  snapshot_endpoint_enabled: true\n"
		"  frame_stats_path: \"/tmp/camsim-frames.jsonl\"\n"));
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	TestEqual(TEXT("cut distance"), Cfg.Render.CameraCutDistanceM, 250.0f);
	TestEqual(TEXT("cut angle"), Cfg.Render.CameraCutAngleDeg, 15.0f);
	TestEqual(TEXT("origin shift distance"), Cfg.Render.OriginShiftDistanceM, 20000.0);
	TestTrue(TEXT("snapshot enabled"), Cfg.Operational.bSnapshotEndpointEnabled);
	TestEqual(TEXT("frame stats path"), Cfg.Operational.FrameStatsPath, FString(TEXT("/tmp/camsim-frames.jsonl")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderConfigEnvTest,
	"CamSim.Config.RenderEnvOverrides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderConfigEnvTest::RunTest(const FString& Parameters)
{
	struct FEnv { const TCHAR* Key; const TCHAR* Value; };
	const FEnv Vars[] = {
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

	TestEqual(TEXT("env cut distance"), Cfg.Render.CameraCutDistanceM, 123.0f);
	TestEqual(TEXT("env cut angle"), Cfg.Render.CameraCutAngleDeg, 12.0f);
	TestEqual(TEXT("env origin shift"), Cfg.Render.OriginShiftDistanceM, 5000.0);
	TestTrue(TEXT("env snapshot"), Cfg.Operational.bSnapshotEndpointEnabled);
	TestEqual(TEXT("env frame stats"), Cfg.Operational.FrameStatsPath, FString(TEXT("/tmp/x.jsonl")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderConfigRelativeFrameStatsPathTest,
	"CamSim.Config.FrameStatsRelativePathUsesLaunchDir",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderConfigRelativeFrameStatsPathTest::RunTest(const FString& Parameters)
{
	// Not the engine's Binaries directory, where UE resolves relative paths.
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
		"operational:\n"
		"  frame_stats_path: \"bench/frames.jsonl\"\n"));
	TestEqual(TEXT("relative path resolved against the launch directory"), Cfg.Operational.FrameStatsPath,
		FPaths::ConvertRelativePathToFull(FPaths::LaunchDir(), TEXT("bench/frames.jsonl")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderConfigLodTransitionsFollowsFlagTest,
	"CamSim.Config.LodTransitionsFollowsFlag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderConfigLodTransitionsFollowsFlagTest::RunTest(const FString& Parameters)
{
	using CamSim::Geospatial::UseLodTransitions;
	FCamSimConfig Cfg;
	// Off by default: Cesium updates the fade of every tile in the render set each
	// frame (including the off-screen tiles culled_screen_space_error keeps), which
	// cost ~6 ms of game thread and doubled streaming hitches (2026-09-27 bench).
	TestFalse(TEXT("off by default"), UseLodTransitions(Cfg));
	Cfg.bUseLodTransitions = true;
	TestTrue(TEXT("on when enabled (TSR resolves the dither)"), UseLodTransitions(Cfg));
	Cfg.bUseLodTransitions = false;
	TestFalse(TEXT("off when disabled"), UseLodTransitions(Cfg));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderConfigCulledSseTest,
	"CamSim.Config.CulledScreenSpaceError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderConfigCulledSseTest::RunTest(const FString& Parameters)
{
	using CamSim::Geospatial::ResolveCulledScreenSpaceError;
	FCamSimConfig Def;
	// Auto (0): off-screen tiles keep the on-screen detail, so a gimbal snap
	// never shows coarse placeholder tiles (Yosemite snap test, 2026-09-27).
	TestEqual(TEXT("0 = auto = maximum_screen_space_error"), ResolveCulledScreenSpaceError(Def),
		static_cast<double>(Def.MaximumScreenSpaceError));
	Def.MaximumScreenSpaceError = 8.0f;
	TestEqual(TEXT("auto follows a changed SSE"), ResolveCulledScreenSpaceError(Def), 8.0);
	TestEqual(TEXT("cache default leaves headroom for the off-screen tiles"), FCamSimConfig().MaximumCachedBytesMB, 2048);

	const FCamSimConfig Yaml = FCamSimConfig::LoadFromYamlString(TEXT("culled_screen_space_error: 24\n"));
	TestEqual(TEXT("yaml value used"), ResolveCulledScreenSpaceError(Yaml), 24.0);
	TestEqual(TEXT("no unknown keys"), Yaml.UnknownYamlKeys.Num(), 0);

	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_CULLED_SSE"), TEXT("48"));
	const FCamSimConfig Env = FCamSimConfig::LoadFromYamlString(TEXT("culled_screen_space_error: 24\n"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_CULLED_SSE"), TEXT(""));
	TestEqual(TEXT("env overrides yaml"), ResolveCulledScreenSpaceError(Env), 48.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderConfigFrustumCullingTest,
	"CamSim.Config.FrustumCulling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderConfigFrustumCullingTest::RunTest(const FString& Parameters)
{
	// Off by default: Cesium applies culled_screen_space_error only to tiles culled
	// by a disabled stage, so frustum culling left snaps on coarse tiles for ~2 s
	// (Yosemite snap test, 2026-09-28).
	TestFalse(TEXT("off by default"), FCamSimConfig().bFrustumCulling);

	const FCamSimConfig Yaml = FCamSimConfig::LoadFromYamlString(TEXT("frustum_culling: true\n"));
	TestTrue(TEXT("yaml value used"), Yaml.bFrustumCulling);
	TestEqual(TEXT("no unknown keys"), Yaml.UnknownYamlKeys.Num(), 0);

	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_FRUSTUM_CULLING"), TEXT("0"));
	const FCamSimConfig Env = FCamSimConfig::LoadFromYamlString(TEXT("frustum_culling: true\n"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_FRUSTUM_CULLING"), TEXT(""));
	TestFalse(TEXT("env overrides yaml"), Env.bFrustumCulling);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderConfigCulledSseFovTest,
	"CamSim.Config.CulledScreenSpaceErrorFollowsFov",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderConfigCulledSseFovTest::RunTest(const FString& Parameters)
{
	using CamSim::Geospatial::ScaleCulledScreenSpaceErrorForFov;
	// Off-screen tiles keep the detail a 60-degree view needs: zooming in must not
	// load zoomed-in detail all the way round (226k UObjects and 0.2-0.8 s GC
	// passes at 10 degrees, 2026-09-29), and wide views keep full detail.
	TestEqual(TEXT("60 deg unchanged"), ScaleCulledScreenSpaceErrorForFov(16.0, 60.0), 16.0, 1e-9);
	TestEqual(TEXT("wider never finer"), ScaleCulledScreenSpaceErrorForFov(16.0, 90.0), 16.0, 1e-9);
	const double Tan30 = FMath::Tan(FMath::DegreesToRadians(30.0));
	TestEqual(TEXT("30 deg"), ScaleCulledScreenSpaceErrorForFov(16.0, 30.0),
		16.0 * Tan30 / FMath::Tan(FMath::DegreesToRadians(15.0)), 1e-9);
	TestEqual(TEXT("10 deg"), ScaleCulledScreenSpaceErrorForFov(16.0, 10.0),
		16.0 * Tan30 / FMath::Tan(FMath::DegreesToRadians(5.0)), 1e-9);
	TestTrue(TEXT("degenerate FOV stays finite"), FMath::IsFinite(ScaleCulledScreenSpaceErrorForFov(16.0, 0.0)));
	TestTrue(TEXT("NaN FOV keeps the base"), ScaleCulledScreenSpaceErrorForFov(16.0, NAN) == 16.0);
	return true;
}
