// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Modules/ModuleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Config/CamSimConfig.h"
#include "GroundTruth/FGroundTruthCollector.h"
#include "Thermal/LandCoverWindow.h"

// -------------------------------------------------------------------------
// Semantic image capture path (ROADMAP 4D): PNG encoding of the class pairs,
// the ml_training.semantic key, the window bind rule, forced file writes.
// -------------------------------------------------------------------------

namespace
{
	TArray<uint32> Pairs4x2()
	{
		// row 0: classes 1, 3, 20, 0; row 1: 7, 8, 2, 25 (pixel 2k in the low half; high 8 bits of each half ignored)
		return { 1u | (3u << 16) | 0xAB00u, 20u | (0u << 16), 7u | (8u << 16), 2u | (25u << 16) | (0xCDu << 24) };
	}

	bool DecodeGray8(const TArray64<uint8>& Png, TArray64<uint8>& Raw, int32& W, int32& H)
	{
		IImageWrapperModule& M = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Wr = M.CreateImageWrapper(EImageFormat::PNG);
		if (!Wr || !Wr->SetCompressed(Png.GetData(), Png.Num())) return false;
		W = Wr->GetWidth();
		H = Wr->GetHeight();
		return Wr->GetRaw(ERGBFormat::Gray, 8, Raw);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSemanticPngTest, "CamSim.GroundTruth.Semantic.Capture.EncodePngRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSemanticPngTest::RunTest(const FString& Parameters)
{
	const TArray64<uint8> Png = FGroundTruthCollector::EncodeSemanticPng(Pairs4x2(), 4, 2);
	TArray64<uint8> Raw;
	int32 W = 0, H = 0;
	if (!TestTrue(TEXT("decodes"), DecodeGray8(Png, Raw, W, H))) return false;
	TestEqual(TEXT("width"), W, 4);
	TestEqual(TEXT("height"), H, 2);
	const uint8 Want[8] = { 1, 3, 20, 0, 7, 8, 2, 25 };
	for (int32 I = 0; I < 8; ++I) TestEqual(FString::Printf(TEXT("pixel %d"), I), Raw[I], Want[I]);
	TestEqual(TEXT("wrong size: empty"), FGroundTruthCollector::EncodeSemanticPng(Pairs4x2(), 6, 2).Num(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSemanticConfigTest, "CamSim.GroundTruth.Semantic.Capture.ConfigKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSemanticConfigTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("off by default"), FCamSimConfig().MLTraining.bSemantic);
	TestTrue(TEXT("yaml"), FCamSimConfig::LoadFromYamlString(TEXT("ml_training:\n  semantic: true\n")).MLTraining.bSemantic);
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_ML_SEMANTIC_ENABLED"), TEXT("1"));
	const FCamSimConfig E = FCamSimConfig::LoadFromYamlString(TEXT("ml_training:\n  semantic: false\n"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CAMSIM_ML_SEMANTIC_ENABLED"), TEXT(""));
	TestTrue(TEXT("env"), E.MLTraining.bSemantic);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSemanticWindowBindTest, "CamSim.GroundTruth.Semantic.Capture.WindowBindRule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSemanticWindowBindTest::RunTest(const FString& Parameters)
{
	// The semantic params carry the window id they were built against; a re-centred GPU window must not be bound.
	TestTrue(TEXT("same id"), CamSimLandCover::ShouldBindWindow(1u, 7u, 7u, true));
	TestFalse(TEXT("re-centred meanwhile"), CamSimLandCover::ShouldBindWindow(1u, 7u, 8u, true));
	TestFalse(TEXT("not uploaded"), CamSimLandCover::ShouldBindWindow(1u, 7u, 7u, false));
	TestFalse(TEXT("land cover off"), CamSimLandCover::ShouldBindWindow(0u, 7u, 7u, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSemanticCollectorTest, "CamSim.GroundTruth.Semantic.Capture.CollectorForceWrites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSemanticCollectorTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Cfg;
	Cfg.MLTraining.bEnabled = true;
	Cfg.MLTraining.bSemantic = true;
	Cfg.MLTraining.AnnotationIntervalFrames = 1000;
	Cfg.MLTraining.OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("SemanticCollector"));
	IFileManager::Get().DeleteDirectory(*Cfg.MLTraining.OutputDir, false, true);
	FGroundTruthCollector C(Cfg);
	if (!TestTrue(TEXT("open"), C.Open())) return false;
	C.WriteSemanticFrame(Pairs4x2(), 4, 2, 5, /*bForce=*/false);
	C.WriteSemanticFrame(Pairs4x2(), 4, 2, 6, /*bForce=*/true);
	C.WriteSemanticFrame(Pairs4x2(), 4, 2, 2000, /*bForce=*/false);
	const FString Dir = FPaths::Combine(Cfg.MLTraining.OutputDir, TEXT("semantic"));
	TestFalse(TEXT("off-interval, not forced: no file"), IFileManager::Get().FileExists(*FPaths::Combine(Dir, TEXT("semantic_00000005.png"))));
	TestTrue(TEXT("forced: written"), IFileManager::Get().FileExists(*FPaths::Combine(Dir, TEXT("semantic_00000006.png"))));
	TestTrue(TEXT("on the interval: written"), IFileManager::Get().FileExists(*FPaths::Combine(Dir, TEXT("semantic_00002000.png"))));
	C.Close();
	return true;
}
