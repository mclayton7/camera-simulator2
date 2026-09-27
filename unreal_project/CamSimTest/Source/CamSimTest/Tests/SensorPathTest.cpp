// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"
#include "SensorFrameParams.h"

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
