// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

// -------------------------------------------------------------------------
// control_api (ROADMAP 2.4): defaults, YAML, env overrides, clamps.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlApiConfigTest,
	"CamSim.ControlApi.Config",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FControlApiConfigTest::RunTest(const FString& Parameters)
{
	const FCamSimConfig D;
	TestFalse(TEXT("off by default"), D.ControlApi.bEnabled);
	TestEqual(TEXT("default settle load"), D.ControlApi.SettleLoadPct, 99.9f);
	TestEqual(TEXT("default settle frames"), D.ControlApi.DefaultSettleFrames, 30);
	TestEqual(TEXT("default capture timeout"), D.ControlApi.CaptureTimeoutSec, 120.0f);

	const FCamSimConfig Y = FCamSimConfig::LoadFromYamlString(TEXT(
		"control_api:\n"
		"  enabled: true\n"
		"  settle_load_pct: 98\n"
		"  default_settle_frames: 5\n"
		"  capture_timeout_s: 9\n"));
	TestTrue(TEXT("yaml enabled"), Y.ControlApi.bEnabled);
	TestEqual(TEXT("yaml settle load"), Y.ControlApi.SettleLoadPct, 98.0f);
	TestEqual(TEXT("yaml settle frames"), Y.ControlApi.DefaultSettleFrames, 5);
	TestEqual(TEXT("yaml timeout"), Y.ControlApi.CaptureTimeoutSec, 9.0f);
	TestEqual(TEXT("no unknown keys"), Y.UnknownYamlKeys.Num(), 0);

	const FCamSimConfig C = FCamSimConfig::LoadFromYamlString(TEXT(
		"control_api:\n"
		"  settle_load_pct: 120\n"
		"  default_settle_frames: 5000\n"
		"  capture_timeout_s: 0\n"));
	TestEqual(TEXT("load pct clamped"), C.ControlApi.SettleLoadPct, 100.0f);
	TestEqual(TEXT("settle frames clamped"), C.ControlApi.DefaultSettleFrames, 600);
	TestEqual(TEXT("timeout clamped"), C.ControlApi.CaptureTimeoutSec, 1.0f);

	struct FEnv { const TCHAR* Key; const TCHAR* Value; };
	const FEnv Vars[] = {
		{ TEXT("CAMSIM_CONTROL_API_ENABLED"),           TEXT("1") },
		{ TEXT("CAMSIM_CONTROL_API_SETTLE_LOAD_PCT"),   TEXT("97.5") },
		{ TEXT("CAMSIM_CONTROL_API_SETTLE_FRAMES"),     TEXT("12") },
		{ TEXT("CAMSIM_CONTROL_API_CAPTURE_TIMEOUT_S"), TEXT("45") },
	};
	for (const FEnv& V : Vars) { FPlatformMisc::SetEnvironmentVar(V.Key, V.Value); }
	const FCamSimConfig E = FCamSimConfig::LoadFromYamlString(TEXT("control_api:\n  enabled: false\n"));
	for (const FEnv& V : Vars) { FPlatformMisc::SetEnvironmentVar(V.Key, TEXT("")); }
	TestTrue(TEXT("env enabled"), E.ControlApi.bEnabled);
	TestEqual(TEXT("env settle load"), E.ControlApi.SettleLoadPct, 97.5f);
	TestEqual(TEXT("env settle frames"), E.ControlApi.DefaultSettleFrames, 12);
	TestEqual(TEXT("env timeout"), E.ControlApi.CaptureTimeoutSec, 45.0f);
	return true;
}
