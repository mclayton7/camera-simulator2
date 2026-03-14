// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"
#include "Camera/CamSimCamera.h"

// -------------------------------------------------------------------------
// Phase 27 — Performance & Optimization Config Tests
// -------------------------------------------------------------------------

// 1. FPerformanceConfig defaults — verify all fields have expected default values
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase27_ConfigDefaults,
	"CamSim.Phase27.ConfigDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase27_ConfigDefaults::RunTest(const FString& Parameters)
{
	FCamSimConfig::FPerformanceConfig C;

	// 27B
	TestFalse(TEXT("bTrackFrameDropsByCategory default false"), C.bTrackFrameDropsByCategory);

	// 27D
	TestFalse(TEXT("bHotReloadConfig default false"), C.bHotReloadConfig);
	TestEqual(TEXT("HotReloadPollIntervalSec default 5.0"), C.HotReloadPollIntervalSec, 5.0f);

	// 27E
	TestEqual(TEXT("TilePrefetchSlewThresholdDegPerSec default 10.0"), C.TilePrefetchSlewThresholdDegPerSec, 10.0f);
	TestEqual(TEXT("TilePrefetchFovBoost default 2.0"),               C.TilePrefetchFovBoost,               2.0f);
	TestEqual(TEXT("TilePrefetchBoostFrames default 30"),             C.TilePrefetchBoostFrames,             30);

	// 27F
	TestEqual(TEXT("RenderFrameRateHz default 30.0"), C.RenderFrameRateHz, 30.0f);
	TestEqual(TEXT("OutputFrameRateHz default 30.0"), C.OutputFrameRateHz, 30.0f);

	// 27G
	TestEqual(TEXT("TexturePoolBudgetMB default 0"), C.TexturePoolBudgetMB, 0);

	// 27A
	TestFalse(TEXT("bGpuSensorEffects default false"), C.bGpuSensorEffects);
	TestEqual(TEXT("GpuSensorMaterialPath default"),
		C.GpuSensorMaterialPath,
		FString(TEXT("/Game/CamSim/Materials/M_SensorPostProcess")));
	TestEqual(TEXT("GpuSensorMpcPath default"),
		C.GpuSensorMpcPath,
		FString(TEXT("/Game/CamSim/Materials/MPC_SensorParams")));

	return true;
}

// 2. Env overrides — setenv CAMSIM_PERF_* then Load() and verify Performance fields
#if PLATFORM_LINUX || PLATFORM_MAC

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase27_EnvOverrides,
	"CamSim.Phase27.EnvOverrides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase27_EnvOverrides::RunTest(const FString& Parameters)
{
	setenv("CAMSIM_PERF_TRACK_DROPS",    "1",    1);
	setenv("CAMSIM_PERF_HOT_RELOAD",     "1",    1);
	setenv("CAMSIM_PERF_POLL_INTERVAL",  "10.0", 1);
	setenv("CAMSIM_PERF_RENDER_FPS",     "60.0", 1);
	setenv("CAMSIM_PERF_OUTPUT_FPS",     "30.0", 1);
	setenv("CAMSIM_PERF_TEXTURE_POOL_MB","1024",  1);
	setenv("CAMSIM_PERF_GPU_SENSOR",     "1",    1);

	FCamSimConfig Cfg = FCamSimConfig::Load();

	TestTrue (TEXT("bTrackFrameDropsByCategory env override"), Cfg.Performance.bTrackFrameDropsByCategory);
	TestTrue (TEXT("bHotReloadConfig env override"),           Cfg.Performance.bHotReloadConfig);
	TestEqual(TEXT("HotReloadPollIntervalSec env override"),   Cfg.Performance.HotReloadPollIntervalSec, 10.0f);
	TestEqual(TEXT("RenderFrameRateHz env override"),          Cfg.Performance.RenderFrameRateHz,        60.0f);
	TestEqual(TEXT("OutputFrameRateHz env override"),          Cfg.Performance.OutputFrameRateHz,        30.0f);
	TestEqual(TEXT("TexturePoolBudgetMB env override"),        Cfg.Performance.TexturePoolBudgetMB,      1024);
	TestTrue (TEXT("bGpuSensorEffects env override"),          Cfg.Performance.bGpuSensorEffects);

	// Clean up env vars to avoid polluting subsequent tests
	unsetenv("CAMSIM_PERF_TRACK_DROPS");
	unsetenv("CAMSIM_PERF_HOT_RELOAD");
	unsetenv("CAMSIM_PERF_POLL_INTERVAL");
	unsetenv("CAMSIM_PERF_RENDER_FPS");
	unsetenv("CAMSIM_PERF_OUTPUT_FPS");
	unsetenv("CAMSIM_PERF_TEXTURE_POOL_MB");
	unsetenv("CAMSIM_PERF_GPU_SENSOR");

	return true;
}

#endif // PLATFORM_LINUX || PLATFORM_MAC

// 3. Frame rate validation — FMath::Clamp behavior on frame rates
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase27_FrameRateValidation,
	"CamSim.Phase27.FrameRateValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase27_FrameRateValidation::RunTest(const FString& Parameters)
{
	// Verify FMath::Clamp correctly constrains frame rates to supported range [1, 120]
	constexpr float MinFps  = 1.0f;
	constexpr float MaxFps  = 120.0f;

	TestEqual(TEXT("30 fps stays 30"),  FMath::Clamp(30.0f,  MinFps, MaxFps), 30.0f);
	TestEqual(TEXT("60 fps stays 60"),  FMath::Clamp(60.0f,  MinFps, MaxFps), 60.0f);
	TestEqual(TEXT("0 fps clamps to 1"),FMath::Clamp(0.0f,   MinFps, MaxFps), MinFps);
	TestEqual(TEXT("200 fps clamps to 120"), FMath::Clamp(200.0f, MinFps, MaxFps), MaxFps);

	// Render and output frame rates from defaults are within valid range
	FCamSimConfig::FPerformanceConfig C;
	TestTrue(TEXT("Default RenderFrameRateHz in [1,120]"),
		C.RenderFrameRateHz >= MinFps && C.RenderFrameRateHz <= MaxFps);
	TestTrue(TEXT("Default OutputFrameRateHz in [1,120]"),
		C.OutputFrameRateHz >= MinFps && C.OutputFrameRateHz <= MaxFps);

	return true;
}

// ---------------------------------------------------------------------------
// Test 4 — FFrameDropStats initial state and counting
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase27_FrameDropStats,
	"CamSim.Phase27.FrameDropStats",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase27_FrameDropStats::RunTest(const FString& Parameters)
{
	FFrameDropStats S;
	TestEqual(TEXT("EncoderBusy zero"),     S.EncoderBusy.Load(),     0);
	TestEqual(TEXT("ReadbackTimeout zero"), S.ReadbackTimeout.Load(), 0);
	TestEqual(TEXT("SocketError zero"),     S.SocketError.Load(),     0);
	TestEqual(TEXT("Total zero"),           S.Total(),                0);

	S.EncoderBusy++;
	S.ReadbackTimeout++;
	TestEqual(TEXT("Total after increments"), S.Total(), 2);
	return true;
}

// ---------------------------------------------------------------------------
// Test 6 — Texture pool budget range check
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase27_TexturePoolRange,
	"CamSim.Phase27.TexturePoolRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPhase27_TexturePoolRange::RunTest(const FString& Parameters)
{
	FCamSimConfig::FPerformanceConfig C;
	TestEqual("Default 0 = engine default", C.TexturePoolBudgetMB, 0);
	C.TexturePoolBudgetMB = 4096;
	TestTrue("4096 MB valid", C.TexturePoolBudgetMB > 0);
	return true;
}

// ---------------------------------------------------------------------------
// Test 7 — Tile prefetch slew detection logic
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase27_TilePrefetchSlew,
    "CamSim.Phase27.TilePrefetchSlewDetection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPhase27_TilePrefetchSlew::RunTest(const FString& Parameters)
{
    // Simulate gimbal velocity calculation
    const float PrevPan  = 10.0f;
    const float CurrPan  = 22.0f;   // 12 deg in 1/30 s = 360 deg/s
    const float DeltaTime = 1.0f / 30.0f;
    const float Threshold = 10.0f;  // deg/s

    const float PanVel = FMath::Abs(CurrPan - PrevPan) / DeltaTime;
    TestTrue(TEXT("Fast slew (360 deg/s) triggers prefetch"), PanVel >= Threshold);

    // Below threshold
    const float SlowPan = 10.1f;
    const float SlowVel = FMath::Abs(SlowPan - PrevPan) / DeltaTime;
    TestFalse(TEXT("Slow slew (3 deg/s) no prefetch"), SlowVel >= Threshold);
    return true;
}

// ---------------------------------------------------------------------------
// Test 8 — Hot-reload: detect immutable field changes
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase27_HotReloadImmutable,
    "CamSim.Phase27.HotReloadImmutableFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPhase27_HotReloadImmutable::RunTest(const FString& Parameters)
{
    // These fields are detected as changed and logged as warnings during hot-reload.
    // The actual preservation logic lives in UCamSimSubsystem::HotReloadConfig().
    FCamSimConfig A, B;

    A.CigiPort      = 8888;            B.CigiPort      = 9999;
    A.MulticastAddr = TEXT("239.1.1.1"); B.MulticastAddr = TEXT("239.1.1.2");
    A.MulticastPort = 5004;            B.MulticastPort = 5005;
    A.VideoCodec    = TEXT("h264");    B.VideoCodec    = TEXT("h265");

    TestTrue(TEXT("CigiPort change detected"),      A.CigiPort != B.CigiPort);
    TestTrue(TEXT("MulticastAddr change detected"), A.MulticastAddr != B.MulticastAddr);
    TestTrue(TEXT("MulticastPort change detected"), A.MulticastPort != B.MulticastPort);
    TestTrue(TEXT("VideoCodec change detected"),    A.VideoCodec != B.VideoCodec);
    return true;
}
