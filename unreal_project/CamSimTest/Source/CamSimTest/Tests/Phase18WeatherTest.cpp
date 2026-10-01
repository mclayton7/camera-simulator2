// Copyright CamSim Contributors. All Rights Reserved.
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase18CloudConfigDefaultsTest,
    "CamSim.Phase18.CloudConfigDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase18CloudConfigDefaultsTest::RunTest(const FString& Parameters)
{
    FCamSimConfig Cfg;
    TestFalse(TEXT("bVolumetricClouds off by default"),       Cfg.Phase18.bVolumetricClouds);
    TestNearlyEqual(TEXT("CloudShadowStrength default 0.6"),  Cfg.Phase18.CloudShadowStrength, 0.6f, 0.001f);
    return true;
}
