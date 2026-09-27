// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"
#include "SensorFrameParams.h"
#include "Sensor/SensorPath.h"
#include "Sensor/SensorStatsMailbox.h"
#include "Config/CamSimConfig.h"

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

namespace
{
	FCamSimConfig CleanGpuConfig()
	{
		FCamSimConfig Cfg;
		for (ESensorMode M : { ESensorMode::EO, ESensorMode::IR, ESensorMode::NVG })
		{
			Cfg.SensorModeConfigs.Add(M, FSensorModeConfig());
			FSensorModeConfig& C = Cfg.SensorModeConfigs[M];
			C.Vignetting = 0.0f;   // default is 0.15: unported in 3B.1
		}
		Cfg.OverlayConfig.bEnabled = false;
		return Cfg;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPathAutoTest, "CamSim.Sensor.Path.AutoPicksGpuOnlyWhenAllPorted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPathAutoTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg = CleanGpuConfig();
	TestTrue(TEXT("clean config -> gpu"), FSensorPathSelector::Decide(Cfg).Path == ESensorPipelinePath::Gpu);

	Cfg.SensorModeConfigs[ESensorMode::IR].NETD = 0.01f;
	Cfg.LaserDesignator.bEnabled = true;
	const FSensorPathDecision D = FSensorPathSelector::Decide(Cfg);
	TestTrue(TEXT("unported -> legacy"), D.Path == ESensorPipelinePath::Legacy);
	TestTrue(TEXT("names noise"), D.Unported.ContainsByPredicate([](const FString& S) { return S.Contains(TEXT("noise_netd")); }));
	TestTrue(TEXT("names laser"), D.Unported.Contains(TEXT("laser_designator")));
	TestTrue(TEXT("reason lists them"), D.Reason.Contains(TEXT("laser_designator")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPathForcedTest, "CamSim.Sensor.Path.ForcedAndSceneCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPathForcedTest::RunTest(const FString& Parameters)
{
	using ESP = FCamSimConfig::FRenderConfig::ESensorPath;
	FCamSimConfig Cfg = CleanGpuConfig();
	Cfg.SensorModeConfigs[ESensorMode::EO].NETD = 0.02f;
	Cfg.Render.SensorPathMode = ESP::Gpu;
	const FSensorPathDecision D = FSensorPathSelector::Decide(Cfg);
	TestTrue(TEXT("forced gpu"), D.Path == ESensorPipelinePath::Gpu);
	TestTrue(TEXT("ignored effects still listed"), D.Unported.Num() == 1 && D.Reason.Contains(TEXT("ignored")));

	Cfg = CleanGpuConfig();
	Cfg.Render.SensorPathMode = ESP::Legacy;
	TestTrue(TEXT("forced legacy"), FSensorPathSelector::Decide(Cfg).Path == ESensorPipelinePath::Legacy);

	Cfg.Render.SensorPathMode = ESP::Gpu;
	Cfg.Render.ViewSourceMode = FCamSimConfig::FRenderConfig::EViewSource::SceneCapture;
	TestTrue(TEXT("scene_capture is always legacy"), FSensorPathSelector::Decide(Cfg).Path == ESensorPipelinePath::Legacy);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPathColorTempTest, "CamSim.Sensor.Path.NeutralValuesAreNotEffects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPathColorTempTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg = CleanGpuConfig();
	Cfg.SensorModeConfigs[ESensorMode::EO].ColorTemperatureK = 6500.0f;  // neutral
	Cfg.SensorModeConfigs[ESensorMode::EO].bAGCEnabled = true;           // ported in 3B.1
	Cfg.SensorModeConfigs[ESensorMode::IR].AGCLagFrames = 2;             // ported in 3B.1
	TestTrue(TEXT("still gpu"), FSensorPathSelector::Decide(Cfg).Path == ESensorPipelinePath::Gpu);
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
