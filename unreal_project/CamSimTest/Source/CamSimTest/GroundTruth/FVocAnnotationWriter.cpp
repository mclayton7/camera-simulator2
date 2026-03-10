// Copyright CamSim Contributors. All Rights Reserved.

#include "GroundTruth/FVocAnnotationWriter.h"
#include "Metadata/KlvBuilder.h"  // FCamSimTelemetry
#include "CamSimTest.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

bool FVocAnnotationWriter::Open(const FString& OutputDir)
{
	if (bIsOpen) return true;

	AnnotationsDir = FPaths::Combine(OutputDir, TEXT("annotations"));
	IFileManager::Get().MakeDirectory(*AnnotationsDir, /*Tree=*/true);

	bIsOpen = true;
	UE_LOG(LogCamSim, Log, TEXT("FVocAnnotationWriter: open -> %s"), *AnnotationsDir);
	return true;
}

void FVocAnnotationWriter::WriteFrame(
    const TArray<FEntityAnnotationData>& Entities,
    const FCamSimTelemetry& /*Telemetry*/,
    uint64 FrameIdx)
{
	if (!bIsOpen) return;

	const FString FileName    = FString::Printf(TEXT("frame_%08llu.xml"), (unsigned long long)FrameIdx);
	const FString ImageName   = FString::Printf(TEXT("frame_%08llu.png"), (unsigned long long)FrameIdx);
	const FString OutFilePath = FPaths::Combine(AnnotationsDir, FileName);

	FString Xml;
	Xml.Reserve(1024);

	Xml += TEXT("<annotation>\n");
	Xml += TEXT("\t<folder>images</folder>\n");
	Xml += FString::Printf(TEXT("\t<filename>%s</filename>\n"), *ImageName);
	Xml += TEXT("\t<source><database>CamSim</database></source>\n");
	Xml += FString::Printf(
		TEXT("\t<size><width>%d</width><height>%d</height><depth>3</depth></size>\n"),
		ImageWidth, ImageHeight);
	Xml += TEXT("\t<segmented>0</segmented>\n");

	for (const FEntityAnnotationData& E : Entities)
	{
		if (!E.bVisible) continue;

		const int32 XMin = FMath::RoundToInt(E.ScreenBBox.Min.X);
		const int32 YMin = FMath::RoundToInt(E.ScreenBBox.Min.Y);
		const int32 XMax = FMath::RoundToInt(E.ScreenBBox.Max.X);
		const int32 YMax = FMath::RoundToInt(E.ScreenBBox.Max.Y);

		if (XMax <= XMin || YMax <= YMin) continue;

		// Escape entity class name for XML safety
		FString SafeName = E.ClassName
			.Replace(TEXT("&"), TEXT("&amp;"))
			.Replace(TEXT("<"), TEXT("&lt;"))
			.Replace(TEXT(">"), TEXT("&gt;"));

		Xml += TEXT("\t<object>\n");
		Xml += FString::Printf(TEXT("\t\t<name>%s</name>\n"), *SafeName);
		Xml += TEXT("\t\t<pose>Unspecified</pose>\n");
		Xml += FString::Printf(TEXT("\t\t<truncated>%d</truncated>\n"), E.bTruncated ? 1 : 0);
		Xml += TEXT("\t\t<difficult>0</difficult>\n");
		Xml += TEXT("\t\t<bndbox>\n");
		Xml += FString::Printf(TEXT("\t\t\t<xmin>%d</xmin>\n"), XMin);
		Xml += FString::Printf(TEXT("\t\t\t<ymin>%d</ymin>\n"), YMin);
		Xml += FString::Printf(TEXT("\t\t\t<xmax>%d</xmax>\n"), XMax);
		Xml += FString::Printf(TEXT("\t\t\t<ymax>%d</ymax>\n"), YMax);
		Xml += TEXT("\t\t</bndbox>\n");
		Xml += TEXT("\t</object>\n");
	}

	Xml += TEXT("</annotation>\n");

	if (!FFileHelper::SaveStringToFile(Xml, *OutFilePath))
	{
		UE_LOG(LogCamSim, Warning,
			TEXT("FVocAnnotationWriter: failed to write %s"), *OutFilePath);
	}
}

void FVocAnnotationWriter::Close()
{
	bIsOpen = false;
}
