// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/IConsoleManager.h"
#include "Environment/CachedExposureBand.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCachedBandSwitchTest, "CamSim.Environment.CachedExposure.Switch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FCachedBandSwitchTest::RunTest(const FString&)
{
	const FCachedExposureBandConfig C;
	FCachedExposureBand B;
	TestEqual(TEXT("starts day"), B.CachedEv(C), 8.0f);
	TestFalse(TEXT("day stays"), B.Update(11.0f, true, C));
	TestTrue(TEXT("dusk → night"), B.Update(-0.5f, true, C));
	TestEqual(TEXT("night ev"), B.CachedEv(C), -6.0f);
	TestFalse(TEXT("IR holds"), B.Update(12.0f, false, C));
	TestTrue(TEXT("still night"), B.IsNight());
	TestFalse(TEXT("inside hysteresis stays night"), B.Update(1.5f, true, C));
	TestTrue(TEXT("dawn → day"), B.Update(2.5f, true, C));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCachedBandChatterTest, "CamSim.Environment.CachedExposure.NoChatter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FCachedBandChatterTest::RunTest(const FString&)
{
	const FCachedExposureBandConfig C;
	FCachedExposureBand B;
	int32 Switches = 0;
	for (int32 I = 0; I < 200; ++I)
	{
		const float Median = -0.4f + 0.9f * FMath::Sin(I * 0.7f);   // oscillates across 0 by ±0.9 EV
		Switches += B.Update(Median, true, C) ? 1 : 0;
	}
	TestEqual(TEXT("one switch, no chatter"), Switches, 1);
	TestFalse(TEXT("NaN median holds"), B.Update(NAN, true, C));
	return true;
}

// A band switch re-encodes the real-time sky capture at the new cached pre-exposure; time-sliced, the old cubemap stayed a
// dozen frames and Lumen's sky lighting dipped ~3 EV for ~1.5 s (night_check.py N5). The hold asks for whole-cubemap
// captures (r.SkyLight.RealTimeReflectionCapture.TimeSlice 0) for FSkyRecaptureHold::Frames ticks after each switch.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkyRecaptureHoldTest, "CamSim.Environment.CachedExposure.RecaptureHoldAfterSwitch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkyRecaptureHoldTest::RunTest(const FString&)
{
	FSkyRecaptureHold Hold;
	TestFalse(TEXT("idle"), Hold.Tick(false));
	TestTrue(TEXT("switch tick"), Hold.Tick(true));
	int32 Held = 1;
	while (Hold.Tick(false)) { ++Held; if (Held > 1000) break; }
	TestEqual(TEXT("held for Frames ticks"), Held, FSkyRecaptureHold::Frames);
	TestTrue(TEXT("a second switch re-arms"), Hold.Tick(true));
	for (int32 I = 0; I < 5; ++I) Hold.Tick(false);
	TestTrue(TEXT("another switch mid-hold restarts the count"), Hold.Tick(true));
	int32 Again = 1;
	while (Hold.Tick(false)) ++Again;
	TestEqual(TEXT("full hold after the restart"), Again, FSkyRecaptureHold::Frames);
	return true;
}

// The environment's wiring of the hold to r.SkyLight.RealTimeReflectionCapture.TimeSlice: 0 during the hold, the previous
// value restored after it and by ReleaseSkyRecaptureHold (EndPlay mid-hold).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkyRecaptureHoldCVarTest, "CamSim.Environment.CachedExposure.RecaptureHoldSetsAndRestoresCVar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkyRecaptureHoldCVarTest::RunTest(const FString&)
{
	IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("r.SkyLight.RealTimeReflectionCapture.TimeSlice"));
	if (!TestNotNull(TEXT("TimeSlice CVar"), Var)) return false;
	const int32 Original = Var->GetInt();
	Var->Set(3, ECVF_SetByCode);
	FSkyRecaptureHoldState State;
	State.Tick(false);
	TestEqual(TEXT("idle: untouched"), Var->GetInt(), 3);
	State.Tick(true);
	TestEqual(TEXT("switch: whole-cubemap captures"), Var->GetInt(), 0);
	for (int32 I = 1; I < FSkyRecaptureHold::Frames; ++I) State.Tick(false);
	TestEqual(TEXT("still held on the last hold tick"), Var->GetInt(), 0);
	State.Tick(false);
	TestEqual(TEXT("restored after the hold"), Var->GetInt(), 3);
	State.Tick(true);
	State.Release();
	TestEqual(TEXT("restored by Release mid-hold"), Var->GetInt(), 3);
	Var->Set(Original, ECVF_SetByCode);
	return true;
}
