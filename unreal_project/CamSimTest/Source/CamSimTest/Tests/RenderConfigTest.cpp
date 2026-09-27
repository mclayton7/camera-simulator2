// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"
#include "Geospatial/CesiumTuning.h"

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
	TestEqual(TEXT("origin shift every 20 km by default"), D.Render.OriginShiftDistanceM, 20000.0);
	TestEqual(TEXT("auto-exposure compensation -1 EV by default"), D.Render.ExposureCompensationEV, -1.0f);
	TestFalse(TEXT("snapshot endpoint off by default"), D.Operational.bSnapshotEndpointEnabled);
	TestTrue(TEXT("frame stats off by default"), D.Operational.FrameStatsPath.IsEmpty());

	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
		"render:\n"
		"  view_source: scene_capture\n"
		"  camera_cut_distance_m: 250.0\n"
		"  camera_cut_angle_deg: 15.0\n"
		"  origin_shift_distance_m: 20000.0\n"
		"  exposure_compensation_ev: -0.5\n"
		"operational:\n"
		"  snapshot_endpoint_enabled: true\n"
		"  frame_stats_path: \"/tmp/camsim-frames.jsonl\"\n"));
	TestEqual(TEXT("no unknown keys"), Cfg.UnknownYamlKeys.Num(), 0);
	TestTrue(TEXT("scene_capture parsed"), Cfg.Render.ViewSourceMode == EViewSource::SceneCapture);
	TestFalse(TEXT("IsPrimary false"), Cfg.Render.IsPrimary());
	TestEqual(TEXT("cut distance"), Cfg.Render.CameraCutDistanceM, 250.0f);
	TestEqual(TEXT("cut angle"), Cfg.Render.CameraCutAngleDeg, 15.0f);
	TestEqual(TEXT("origin shift distance"), Cfg.Render.OriginShiftDistanceM, 20000.0);
	TestEqual(TEXT("exposure compensation"), Cfg.Render.ExposureCompensationEV, -0.5f);
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
		{ TEXT("CAMSIM_RENDER_EXPOSURE_COMPENSATION_EV"), TEXT("-1.5") },
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
	TestEqual(TEXT("env exposure compensation"), Cfg.Render.ExposureCompensationEV, -1.5f);
	return true;
}

// Hot reload can't switch the render path: the grab extension, AA, viewport
// rendering, Cesium cameras and origin shift are all set up at BeginPlay.
// A reload keeps the running values (final review, 3A).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderConfigHotReloadTest,
	"CamSim.Config.HotReloadKeepsRestartOnlySettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderConfigHotReloadTest::RunTest(const FString& Parameters)
{
	using ESP = FCamSimConfig::FRenderConfig::ESensorPath;
	FCamSimConfig Running;
	Running.CigiPort = 8888;
	Running.Render.OriginShiftDistanceM = 20000.0;
	Running.CaptureWidth = 1280;
	Running.CaptureHeight = 720;
	Running.Render.SensorPath = TEXT("gpu");
	Running.Render.SensorPathMode = ESP::Gpu;

	FCamSimConfig Reloaded;
	Reloaded.CigiPort = 9999;
	Reloaded.Render.ViewSource = TEXT("scene_capture");
	Reloaded.Render.ViewSourceMode = FCamSimConfig::FRenderConfig::EViewSource::SceneCapture;
	Reloaded.Render.OriginShiftDistanceM = 5000.0;
	Reloaded.Render.CameraCutAngleDeg = 12.0f;   // live-tunable: takes effect
	// ROADMAP 3B: capture size and sensor path size the readback buffers and the
	// encoder, and are chosen once per session.
	Reloaded.CaptureWidth = 1920;
	Reloaded.CaptureHeight = 1080;
	Reloaded.Render.SensorPath = TEXT("legacy");
	Reloaded.Render.SensorPathMode = ESP::Legacy;

	FCamSimConfig::KeepRestartOnlySettings(Running, Reloaded);

	TestEqual(TEXT("CIGI port kept"), Reloaded.CigiPort, 8888);
	TestTrue(TEXT("view source kept"), Reloaded.Render.IsPrimary());
	TestEqual(TEXT("view source name kept"), Reloaded.Render.ViewSource, FString(TEXT("primary")));
	TestEqual(TEXT("origin shift kept"), Reloaded.Render.OriginShiftDistanceM, 20000.0);
	TestEqual(TEXT("camera cut threshold reloads"), Reloaded.Render.CameraCutAngleDeg, 12.0f);
	TestEqual(TEXT("capture width kept"), Reloaded.CaptureWidth, 1280);
	TestEqual(TEXT("capture height kept"), Reloaded.CaptureHeight, 720);
	TestEqual(TEXT("sensor path name kept"), Reloaded.Render.SensorPath, FString(TEXT("gpu")));
	TestTrue(TEXT("sensor path mode kept"), Reloaded.Render.SensorPathMode == ESP::Gpu);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderConfigLodTransitionsPrimaryOnlyTest,
	"CamSim.Config.LodTransitionsOnlyInPrimaryView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderConfigLodTransitionsPrimaryOnlyTest::RunTest(const FString& Parameters)
{
	using CamSim::Geospatial::UseLodTransitions;
	using EViewSource = FCamSimConfig::FRenderConfig::EViewSource;
	FCamSimConfig Cfg;
	TestTrue(TEXT("on by default in the primary view (TSR resolves the dither)"), UseLodTransitions(Cfg));
	Cfg.Render.ViewSourceMode = EViewSource::SceneCapture;
	TestFalse(TEXT("off with scene_capture (FXAA would blur it), keeping the A/B baseline unchanged"), UseLodTransitions(Cfg));
	Cfg.Render.ViewSourceMode = EViewSource::Primary;
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
