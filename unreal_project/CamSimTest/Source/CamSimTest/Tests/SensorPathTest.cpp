// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"
#include "SensorFrameParams.h"
#include "Sensor/SensorStatsMailbox.h"
#include "Config/CamSimConfig.h"
#include "Subsystem/CamSimSubsystem.h"
#include "RHIGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorShaderModuleTest,
	"CamSim.Sensor.ShaderModule.LoadedWithShaderDirectory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorShaderModuleTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("CamSimShaders loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("CamSimShaders")));
	TestTrue(TEXT("/CamSim shader directory mapped"), AllShaderSourceDirectoryMappings().Contains(TEXT("/CamSim")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorHistogramBinTest,
	"CamSim.Sensor.Histogram.BinOf",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSensorHistogramBinTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("zero"), FSensorHistogram::BinOf(0.0f), 0);
	TestEqual(TEXT("negative"), FSensorHistogram::BinOf(-1.0f), 0);
	TestEqual(TEXT("NaN"), FSensorHistogram::BinOf(NAN), 0);
	TestEqual(TEXT("1.0 is stop 16"), FSensorHistogram::BinOf(1.0f), 128);
	TestEqual(TEXT("huge clamps"), FSensorHistogram::BinOf(1e30f), 255);
	for (int32 B = 0; B < FSensorHistogram::NumBins; ++B)
	{
		TestEqual(FString::Printf(TEXT("centre of bin %d"), B),
			FSensorHistogram::BinOf(FMath::Exp2(FSensorHistogram::BinCentreLog2(B))), B);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorLegacyKeysGoneTest, "CamSim.Sensor.Config.LegacyPathKeysUnknown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorLegacyKeysGoneTest::RunTest(const FString& Parameters)
{
	AddExpectedMessage(TEXT("in <string> is ignored"), EAutomationExpectedErrorFlags::Contains, 3);
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT(
		"render:\n  sensor_path: legacy\n"
		"overlay:\n  enabled: true\n"
		"performance:\n  gpu_sensor_effects: true\n"));
	for (const TCHAR* Key : { TEXT("sensor_path"), TEXT("overlay"), TEXT("gpu_sensor_effects") })
	{
		TestTrue(FString::Printf(TEXT("%s reported unknown"), Key),
			Cfg.UnknownYamlKeys.ContainsByPredicate([Key](const FString& K) { return K.Contains(Key); }));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGraphPreconditionsTest, "CamSim.Sensor.Graph.ConfigPreconditions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGraphPreconditionsTest::RunTest(const FString& Parameters)
{
	// The config checks run before the RHI check, so they are testable under NullRHI.
	FCamSimConfig Cfg;
	Cfg.CaptureWidth = 1366;   // even, not a multiple of 4
	Cfg.CaptureHeight = 768;
	FString Why;
	TestFalse(TEXT("1366 wide -> unavailable"), UCamSimSubsystem::CanRunSensorGraph(Cfg, Why));
	TestTrue(TEXT("reason names the width"), Why.Contains(TEXT("1366")));

	Cfg.CaptureWidth = 1280;
	Cfg.CaptureHeight = 721;
	Why.Reset();
	TestFalse(TEXT("odd height -> unavailable"), UCamSimSubsystem::CanRunSensorGraph(Cfg, Why));
	TestTrue(TEXT("reason names the height"), Why.Contains(TEXT("721")));

	Cfg.CaptureHeight = 720;
	Why.Reset();
	const bool bOk = UCamSimSubsystem::CanRunSensorGraph(Cfg, Why);
	if (GUsingNullRHI)
	{
		TestFalse(TEXT("NullRHI -> unavailable"), bOk);
		TestTrue(TEXT("reason names NullRHI"), Why.Contains(TEXT("NullRHI")));
	}
	else
	{
		TestTrue(FString::Printf(TEXT("real RHI, valid config -> available (%s)"), *Why), bOk);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorMailboxTest, "CamSim.Sensor.Mailbox.NewestWins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorMailboxTest::RunTest(const FString& Parameters)
{
	FSensorStatsMailbox Box;
	FSensorHistogram Out;
	TestFalse(TEXT("empty"), Box.TakeLatest(Out));
	FSensorHistogram A; A.Serial = 1; Box.Publish(A);
	FSensorHistogram B; B.Serial = 2; Box.Publish(B);
	TestTrue(TEXT("has one"), Box.TakeLatest(Out));
	TestEqual(TEXT("newest"), Out.Serial, 2u);
	TestFalse(TEXT("consumed"), Box.TakeLatest(Out));
	return true;
}
