// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/IConsoleManager.h"
#include "Camera/ThermalTsrAlpha.h"

// CamSim.Thermal.TsrAlpha.*: r.TSR.AlphaChannel = 1 (RGBA16F TSR output and history) only while thermal IR runs
// (ROADMAP 4A Task 17 fix round 1), restored afterwards, never clobbering a higher-priority user override.

namespace
{
	/** A scratch cvar with r.TSR.AlphaChannel's default and flags, so the test never touches the renderer's. One per test:
	 *  a cvar keeps the highest priority it was ever set at. */
	IConsoleVariable* ScratchVar(const TCHAR* Name)
	{
		IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
		return Var ? Var : IConsoleManager::Get().RegisterConsoleVariable(Name, -1, TEXT("CamSim.Thermal.TsrAlpha test scratch"),
			ECVF_RenderThreadSafe);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalTsrAlphaToggleTest, "CamSim.Thermal.TsrAlpha.ToggleAndRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalTsrAlphaToggleTest::RunTest(const FString& Parameters)
{
	IConsoleVariable* Var = ScratchVar(TEXT("camsim.Test.TsrAlphaScratchA"));
	Var->Set(-1, ECVF_SetByCode);
	FThermalTsrAlpha Ov;
	Ov.Update(false, Var);
	TestEqual(TEXT("EO before any thermal frame: untouched"), Var->GetInt(), -1);
	Ov.Update(true, Var);
	TestEqual(TEXT("thermal: alpha channel on"), Var->GetInt(), 1);
	TestTrue(TEXT("thermal: applied"), Ov.IsApplied());
	Ov.Update(true, Var);
	TestEqual(TEXT("thermal, steady"), Var->GetInt(), 1);
	Ov.Update(false, Var);
	TestEqual(TEXT("back to EO: restored"), Var->GetInt(), -1);
	TestFalse(TEXT("back to EO: not applied"), Ov.IsApplied());

	// A value already set (e.g. an ini / command-line 0) is saved and restored, not replaced by -1.
	Var->Set(0, ECVF_SetByCode);
	Ov.Update(true, Var);
	TestEqual(TEXT("override saved, thermal on"), Var->GetInt(), 1);
	Ov.Restore(Var);
	TestEqual(TEXT("Restore (shutdown) puts the saved value back"), Var->GetInt(), 0);
	Var->Set(-1, ECVF_SetByCode);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalTsrAlphaUserOverrideTest, "CamSim.Thermal.TsrAlpha.RespectsConsoleOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalTsrAlphaUserOverrideTest::RunTest(const FString& Parameters)
{
	IConsoleVariable* Var = ScratchVar(TEXT("camsim.Test.TsrAlphaScratchB"));
	// Registered fresh in each editor process (a cvar keeps the highest priority it was set at: this test runs once per process).
	FThermalTsrAlpha Ov;
	Ov.Update(true, Var);
	TestEqual(TEXT("thermal: alpha channel on"), Var->GetInt(), 1);
	// The user changes it from the console while thermal runs (higher priority than code): leave theirs alone.
	Var->Set(0, ECVF_SetByConsole);
	Ov.Update(false, Var);
	TestEqual(TEXT("a console value set during thermal survives the switch back"), Var->GetInt(), 0);
	TestFalse(TEXT("not applied"), Ov.IsApplied());
	// Next thermal entry: SetByCode is below SetByConsole, so the user's 0 stands (the console manager warns once).
	AddExpectedMessagePlain(TEXT("was ignored as it is lower priority than the previous 'SetByConsole'"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	Ov.Update(true, Var);
	TestEqual(TEXT("console override not clobbered"), Var->GetInt(), 0);
	Ov.Update(false, Var);
	TestEqual(TEXT("console override still in place"), Var->GetInt(), 0);
	return true;
}
