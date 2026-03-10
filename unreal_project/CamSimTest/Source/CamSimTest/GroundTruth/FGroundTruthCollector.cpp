// Copyright CamSim Contributors. All Rights Reserved.

#include "GroundTruth/FGroundTruthCollector.h"
#include "GroundTruth/FCocoAnnotationWriter.h"
#include "GroundTruth/FVocAnnotationWriter.h"
#include "GroundTruth/FDepthMapWriter.h"
#include "Config/CamSimConfig.h"
#include "Metadata/KlvBuilder.h"  // FCamSimTelemetry
#include "CamSimTest.h"

#include "Misc/Paths.h"
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
			if (Coco->Open(OutputDir))
				Writers.Add(MoveTemp(Coco));
		}
		if (Config.MLTraining.bVocExport)
		{
			auto Voc = MakeUnique<FVocAnnotationWriter>();
			Voc->SetImageSize(Config.CaptureWidth, Config.CaptureHeight);
			if (Voc->Open(OutputDir))
				Writers.Add(MoveTemp(Voc));
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

	bIsOpen = true;
	UE_LOG(LogCamSim, Log,
		TEXT("FGroundTruthCollector: opened — depth=%d bbox=%d coco=%d voc=%d interval=%d"),
		Config.MLTraining.bDepthMap ? 1 : 0,
		Config.MLTraining.bBoundingBoxes ? 1 : 0,
		Config.MLTraining.bCocoExport ? 1 : 0,
		Config.MLTraining.bVocExport  ? 1 : 0,
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

void FGroundTruthCollector::SetPendingEntitySnapshot(
    TArray<FEntityAnnotationData> Entities, int32 ImageWidth, int32 ImageHeight)
{
	PendingEntities    = MoveTemp(Entities);
	PendingImageWidth  = ImageWidth;
	PendingImageHeight = ImageHeight;
}

void FGroundTruthCollector::WriteAnnotationFrame(
    const FCamSimTelemetry& Telemetry, uint64 FrameIdx)
{
	if (!bIsOpen || !bEnabled || Writers.IsEmpty()) return;
	if ((FrameIdx % static_cast<uint64>(AnnotationIntervalFrames)) != 0) return;

	// Filter to only visible entities before handing off to writers.
	// Writers may still skip zero-area boxes, but invisible entities must never
	// appear in annotation output (they were occluded or outside the frustum).
	TArray<FEntityAnnotationData> VisibleEntities;
	VisibleEntities.Reserve(PendingEntities.Num());
	for (const FEntityAnnotationData& E : PendingEntities)
	{
		if (E.bVisible) VisibleEntities.Add(E);
	}

	for (auto& W : Writers)
	{
		if (W && W->IsOpen())
			W->WriteFrame(VisibleEntities, Telemetry, FrameIdx);
	}
}

void FGroundTruthCollector::WriteDepthFrame(
    const TArray<float>& DepthMetres, int32 Width, int32 Height, uint64 FrameIdx)
{
	if (!bIsOpen || !bEnabled || !DepthWriter || !DepthWriter->IsOpen()) return;
	if ((FrameIdx % static_cast<uint64>(AnnotationIntervalFrames)) != 0) return;

	DepthWriter->WriteDepthFrame(DepthMetres, Width, Height, FrameIdx);
}
