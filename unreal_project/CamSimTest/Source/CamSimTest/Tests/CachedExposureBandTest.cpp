// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
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
