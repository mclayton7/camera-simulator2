// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Config/CamSimConfig.h"

// -------------------------------------------------------------------------
// Phase 28D: Config Validation Tests
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase28ValidConfigTest,
	"CamSim.Phase28.Config.ValidConfig_ReturnsNoErrors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase28ValidConfigTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	TArray<FString> Errors = Cfg.Validate();
	TestEqual(TEXT("Default config has no validation errors"), Errors.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase28InvalidPortTest,
	"CamSim.Phase28.Config.InvalidPort_ReturnsError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase28InvalidPortTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.MulticastPort = 99999;
	TArray<FString> Errors = Cfg.Validate();
	TestTrue(TEXT("Errors list is non-empty"), Errors.Num() > 0);
	bool bFoundPortError = false;
	for (const FString& E : Errors)
	{
		if (E.Contains(TEXT("MulticastPort")))
		{
			bFoundPortError = true;
			break;
		}
	}
	TestTrue(TEXT("Error mentions MulticastPort"), bFoundPortError);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase28InvalidResolutionTest,
	"CamSim.Phase28.Config.InvalidResolution_ReturnsError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase28InvalidResolutionTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.CaptureWidth = 0;
	TArray<FString> Errors = Cfg.Validate();
	TestTrue(TEXT("Has errors for width=0"), Errors.Num() > 0);
	bool bFoundWidth = false;
	for (const FString& E : Errors)
	{
		if (E.Contains(TEXT("CaptureWidth")))
		{
			bFoundWidth = true;
			break;
		}
	}
	TestTrue(TEXT("Error mentions CaptureWidth"), bFoundWidth);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase28GimbalLimitsTest,
	"CamSim.Phase28.Config.GimbalLimitsInverted_ReturnsError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase28GimbalLimitsTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.GimbalPitchMin = 30.0f;
	Cfg.GimbalPitchMax = -90.0f; // inverted
	TArray<FString> Errors = Cfg.Validate();
	bool bFoundGimbal = false;
	for (const FString& E : Errors)
	{
		if (E.Contains(TEXT("GimbalPitch")))
		{
			bFoundGimbal = true;
			break;
		}
	}
	TestTrue(TEXT("Error mentions GimbalPitch"), bFoundGimbal);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase28OddWidthTest,
	"CamSim.Phase28.Config.OddWidth_ReturnsError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase28OddWidthTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.CaptureWidth = 1921; // odd
	TArray<FString> Errors = Cfg.Validate();
	bool bFoundEven = false;
	for (const FString& E : Errors)
	{
		if (E.Contains(TEXT("even")))
		{
			bFoundEven = true;
			break;
		}
	}
	TestTrue(TEXT("Error mentions even requirement"), bFoundEven);
	return true;
}

// -------------------------------------------------------------------------
// Phase 28B: Structured JSON Logger Tests
// -------------------------------------------------------------------------

#include "Logging/CamSimJsonLogger.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase28LoggerWritesJsonTest,
	"CamSim.Phase28.Logger.WritesValidJsonLine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase28LoggerWritesJsonTest::RunTest(const FString& Parameters)
{
	const FString TestPath = FPaths::Combine(
		FPlatformProcess::UserTempDir(), TEXT("camsim_test_log.jsonl"));

	FCamSimJsonLogger Logger;
	TestTrue(TEXT("Logger opens"), Logger.Open(TestPath));

	TMap<FString, FString> Fields;
	Fields.Add(TEXT("codec"), TEXT("h264"));
	Logger.Log(TEXT("info"), TEXT("encoder"), TEXT("opened"), Fields);
	Logger.Flush();
	Logger.Close();

	FString Contents;
	TestTrue(TEXT("File exists"), FFileHelper::LoadFileToString(Contents, *TestPath));
	TestTrue(TEXT("Contains severity"), Contents.Contains(TEXT("\"severity\":\"info\"")));
	TestTrue(TEXT("Contains category"), Contents.Contains(TEXT("\"category\":\"encoder\"")));
	TestTrue(TEXT("Contains msg"), Contents.Contains(TEXT("\"msg\":\"opened\"")));
	TestTrue(TEXT("Contains codec field"), Contents.Contains(TEXT("\"codec\":\"h264\"")));
	TestTrue(TEXT("Contains ts"), Contents.Contains(TEXT("\"ts\":")));

	// Clean up
	IFileManager::Get().Delete(*TestPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhase28LoggerDisabledTest,
	"CamSim.Phase28.Logger.DisabledLogger_NoFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhase28LoggerDisabledTest::RunTest(const FString& Parameters)
{
	FCamSimJsonLogger Logger;
	// Don't call Open — logger should be a no-op
	Logger.Log(TEXT("info"), TEXT("test"), TEXT("should not crash"));
	TestFalse(TEXT("Logger is not open"), Logger.IsOpen());
	return true;
}
