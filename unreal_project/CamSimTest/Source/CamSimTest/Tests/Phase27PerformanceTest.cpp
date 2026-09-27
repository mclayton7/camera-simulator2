// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"
#include "Camera/CamSimCamera.h"
#include "HAL/PlatformMemory.h"
#include "Metadata/KlvBuilder.h"

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
	// Default 1.0 = "no boost" — also matches deploy/camsim_config.yaml. The use
	// site (CamSimCamera.cpp) clamps to >= 1.0 anyway, so 1.0 is the meaningful
	// disabled-by-default value.
	TestEqual(TEXT("TilePrefetchFovBoost default 1.0"),               C.TilePrefetchFovBoost,               1.0f);
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
// Phase 2 — KLV per-frame allocation regression net
//
// FKlvBuilder::BuildMisbST0601Into runs once per encoded frame on the encoder
// thread. Before Phase 2 it allocated a fresh TArray<uint8> Value; per call.
// After Phase 2 the scratch is thread_local and amortises across calls.
//
// This test runs the builder in a tight loop and watches the process's used
// heap counter. After a few warmup iterations the per-call heap delta must
// be near-zero (occasional spikes from OS allocator rebalancing are tolerated
// via the byte-budget threshold below).
//
// The placeholder budget = 0 means the test is informational until CI fills
// in a real budget that survives the inevitable allocator noise. Real values
// observed during Phase 2 CI runs go into kPhase2KlvBytesPerCallBudget below.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase2_KlvAllocationBudget,
    "CamSim.Phase2.KlvAllocationBudget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPhase2_KlvAllocationBudget::RunTest(const FString& Parameters)
{
    constexpr int32 kWarmupIters = 32;
    constexpr int32 kMeasuredIters = 256;

    // Set this to a non-zero value once you have a steady-state baseline from
    // CI runs. Until then the assertion is informational only.
    constexpr uint64 kPhase2KlvBytesPerCallBudget = 0;

    FCamSimTelemetry T;
    T.TimestampUs = 1700000000000000ULL;
    T.Latitude = 37.7749; T.Longitude = -122.4194; T.Altitude = 100.0;
    T.HFovDeg = 60.0f;    T.VFovDeg = 33.75f;

    TArray<uint8> Packet;
    Packet.Reserve(512);

    // Warmup so thread_local scratch grows to its steady-state capacity.
    for (int32 i = 0; i < kWarmupIters; ++i)
    {
        FKlvBuilder::BuildMisbST0601Into(T, Packet);
    }

    const SIZE_T UsedBefore = FPlatformMemory::GetStats().UsedPhysical;

    for (int32 i = 0; i < kMeasuredIters; ++i)
    {
        FKlvBuilder::BuildMisbST0601Into(T, Packet);
    }

    const SIZE_T UsedAfter = FPlatformMemory::GetStats().UsedPhysical;
    const int64  Delta = static_cast<int64>(UsedAfter) - static_cast<int64>(UsedBefore);
    const int64  BytesPerCall = (Delta > 0) ? (Delta / kMeasuredIters) : 0;

    AddInfo(FString::Printf(TEXT("KLV bytes/call after %d iters: %lld (delta=%lld bytes)"),
                            kMeasuredIters, (long long)BytesPerCall, (long long)Delta));

    if (kPhase2KlvBytesPerCallBudget > 0)
    {
        TestTrue(
            FString::Printf(TEXT("Phase 2 KLV: %lld bytes/call ≤ %llu (budget)"),
                            (long long)BytesPerCall, (unsigned long long)kPhase2KlvBytesPerCallBudget),
            BytesPerCall <= static_cast<int64>(kPhase2KlvBytesPerCallBudget));
    }
    else
    {
        AddInfo(TEXT("kPhase2KlvBytesPerCallBudget = 0 (placeholder); update once CI baseline is known."));
    }

    return true;
}
