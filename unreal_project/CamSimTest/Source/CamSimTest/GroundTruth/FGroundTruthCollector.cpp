// Copyright CamSim Contributors. All Rights Reserved.

#include "GroundTruth/FGroundTruthCollector.h"
#include "GroundTruth/FCocoAnnotationWriter.h"
#include "GroundTruth/FDepthMapWriter.h"
#include "GroundTruth/InstanceMaskAnalyzer.h"
#include "Config/CamSimConfig.h"
#include "Metadata/KlvBuilder.h"  // FCamSimTelemetry
#include "CamSimTest.h"

#include "Misc/Paths.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "Modules/ModuleManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformProcess.h"

FGroundTruthCollector::FGroundTruthCollector(const FCamSimConfig& InConfig)
	: Config(InConfig)
{
}

FGroundTruthCollector::~FGroundTruthCollector()
{
	Close();
}

bool FGroundTruthCollector::Open()
{
	if (bIsOpen) return true;

	bEnabled = Config.MLTraining.bEnabled;
	if (!bEnabled) return true;

	AnnotationIntervalFrames = FMath::Max(1, Config.MLTraining.AnnotationIntervalFrames);
	MinVisiblePixels = Config.MLTraining.MinVisiblePixels;
	bSegmentation = Config.MLTraining.bSegmentation;
	CaptureSize = FIntPoint(Config.CaptureWidth, Config.CaptureHeight);
	DepthFarPlaneM = FMath::Max(Config.MLTraining.DepthFarPlaneM, 1.0f);
	bSemanticFiles = Config.MLTraining.bSemantic;

	// Resolve output directory
	FString OutputDir = Config.MLTraining.OutputDir;
	if (OutputDir.IsEmpty())
	{
		OutputDir = FPaths::Combine(FPlatformProcess::BaseDir(), TEXT("ml_output"));
	}
	if (FPaths::IsRelative(OutputDir))
	{
		OutputDir = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPlatformProcess::BaseDir(), OutputDir));
	}

	UE_LOG(LogCamSim, Log, TEXT("FGroundTruthCollector: output dir = %s"), *OutputDir);

	// Annotation writers
	if (Config.MLTraining.bBoundingBoxes)
	{
		if (Config.MLTraining.bCocoExport)
		{
			auto Coco = MakeUnique<FCocoAnnotationWriter>();
			Coco->SetImageSize(Config.CaptureWidth, Config.CaptureHeight);
			if (Coco->Open(OutputDir))
				Writers.Add(MoveTemp(Coco));
		}
	}

	// Depth writer
	if (Config.MLTraining.bDepthMap)
	{
		DepthWriter = MakeUnique<FDepthMapWriter>();
		if (!DepthWriter->Open(OutputDir, Config.MLTraining.DepthFarPlaneM))
		{
			UE_LOG(LogCamSim, Warning, TEXT("FGroundTruthCollector: depth writer failed to open"));
			DepthWriter.Reset();
		}
	}

	if (bSemanticFiles)
	{
		SemanticDir = FPaths::Combine(OutputDir, TEXT("semantic"));
		IFileManager::Get().MakeDirectory(*SemanticDir, /*Tree=*/true);
		// PNG encoding runs on task threads: load ImageWrapper here, on the game thread.
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	}
	bIsOpen = true;
	UE_LOG(LogCamSim, Log,
		TEXT("FGroundTruthCollector: opened — depth=%d bbox=%d coco=%d interval=%d"),
		Config.MLTraining.bDepthMap ? 1 : 0,
		Config.MLTraining.bBoundingBoxes ? 1 : 0,
		Config.MLTraining.bCocoExport ? 1 : 0,
		AnnotationIntervalFrames);
	return true;
}

void FGroundTruthCollector::Close()
{
	for (auto& W : Writers) { if (W) W->Close(); }
	Writers.Empty();
	if (DepthWriter) { DepthWriter->Close(); DepthWriter.Reset(); }
	bIsOpen  = false;
	bEnabled = false;
}

FString FGroundTruthCollector::WriteAnnotationFrame(
    TArray<FEntityAnnotationData> Entities, const FInstanceIdImage* Ids,
    const FCamSimTelemetry& Telemetry, uint64 FrameIdx, bool bForce)
{
	if (!bIsOpen || !bEnabled) return FString();
	if (!bForce && (Writers.IsEmpty() || (FrameIdx % static_cast<uint64>(AnnotationIntervalFrames)) != 0)) return FString();

	// Measure from the rendered ID image; the size check guards an ID image of another size.
	if (Ids && Ids->IsValid() && Ids->Width == CaptureSize.X && Ids->Height == CaptureSize.Y)
	{
		FInstanceMaskAnalyzer::Analyze(*Ids, Entities, MinVisiblePixels, bSegmentation);
	}

	// Only visible entities reach the writers (occluded or outside the frustum: never annotated).
	Entities.RemoveAll([](const FEntityAnnotationData& E) { return !E.bVisible; });

	for (auto& W : Writers)
	{
		if (W && W->IsOpen())
			W->WriteFrame(Entities, Telemetry, FrameIdx);
	}
	return bForce ? FCocoAnnotationWriter::BuildFrameRecord(Entities, Telemetry, FrameIdx, CaptureSize.X, CaptureSize.Y)
	              : FString();
}

void FGroundTruthCollector::WriteDepthFrame(
    const TArray<float>& DepthMetres, int32 Width, int32 Height, uint64 FrameIdx, bool bForce)
{
	if (!bIsOpen || !bEnabled || !DepthWriter || !DepthWriter->IsOpen()) return;
	if (!bForce && (FrameIdx % static_cast<uint64>(AnnotationIntervalFrames)) != 0) return;

	DepthWriter->WriteDepthFrame(DepthMetres, Width, Height, FrameIdx);
}

void FGroundTruthCollector::WriteSemanticFrame(const TArray<uint32>& Pairs, int32 Width, int32 Height, uint64 FrameIdx, bool bForce)
{
	if (!bIsOpen || !bEnabled || !bSemanticFiles || SemanticDir.IsEmpty()) return;
	if (!bForce && (FrameIdx % static_cast<uint64>(AnnotationIntervalFrames)) != 0) return;
	const TArray64<uint8> Png = EncodeSemanticPng(Pairs, Width, Height);
	if (Png.IsEmpty())
	{
		UE_LOG(LogCamSim, Warning, TEXT("GroundTruth: semantic PNG failed for frame %llu"), (unsigned long long)FrameIdx);
		return;
	}
	const FString Path = FPaths::Combine(SemanticDir, FString::Printf(TEXT("semantic_%08llu.png"), (unsigned long long)FrameIdx));
	if (!FFileHelper::SaveArrayToFile(Png, *Path))
	{
		UE_LOG(LogCamSim, Warning, TEXT("GroundTruth: failed to write %s"), *Path);
	}
}

TArray64<uint8> FGroundTruthCollector::EncodeSemanticPng(const TArray<uint32>& Pairs, int32 Width, int32 Height)
{
	if (Width <= 0 || Height <= 0 || Width % 2 != 0 || Pairs.Num() != Width * Height / 2) return {};
	IImageWrapperModule* Module = FModuleManager::GetModulePtr<IImageWrapperModule>(TEXT("ImageWrapper"));
	if (!Module && IsInGameThread()) Module = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	if (!Module) return {};
	TArray<uint8> Gray;
	Gray.SetNumUninitialized(Width * Height);
	for (int32 I = 0; I < Pairs.Num(); ++I)
	{
		Gray[2 * I]     = static_cast<uint8>(Pairs[I] & 0xFFu);
		Gray[2 * I + 1] = static_cast<uint8>((Pairs[I] >> 16) & 0xFFu);
	}
	TSharedPtr<IImageWrapper> Png = Module->CreateImageWrapper(EImageFormat::PNG);
	if (!Png || !Png->SetRaw(Gray.GetData(), Gray.Num(), Width, Height, ERGBFormat::Gray, 8)) return {};
	return Png->GetCompressed(0);
}
