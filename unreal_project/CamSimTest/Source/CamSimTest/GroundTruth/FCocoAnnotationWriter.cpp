// Copyright CamSim Contributors. All Rights Reserved.

#include "GroundTruth/FCocoAnnotationWriter.h"
#include "Metadata/KlvBuilder.h"  // FCamSimTelemetry
#include "CamSimTest.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"

bool FCocoAnnotationWriter::Open(const FString& OutputDir)
{
	if (bIsOpen) return true;

	IFileManager::Get().MakeDirectory(*OutputDir, /*Tree=*/true);

	JsonlPath = FPaths::Combine(OutputDir, TEXT("camsim_coco.jsonl"));

	// Create / truncate the file and keep a persistent append handle.
	// Using a persistent IFileHandle avoids per-frame open+close+seek overhead
	// that would otherwise accumulate on multi-hour capture sessions.
	FileHandle = IFileManager::Get().CreateFileWriter(*JsonlPath, FILEWRITE_AllowRead);
	if (!FileHandle)
	{
		UE_LOG(LogCamSim, Error, TEXT("FCocoAnnotationWriter: failed to create %s"), *JsonlPath);
		return false;
	}

	bIsOpen = true;
	UE_LOG(LogCamSim, Log, TEXT("FCocoAnnotationWriter: open -> %s"), *JsonlPath);
	return true;
}

void FCocoAnnotationWriter::WriteFrame(
    const TArray<FEntityAnnotationData>& Entities,
    const FCamSimTelemetry& Telemetry,
    uint64 FrameIdx)
{
	if (!bIsOpen || !FileHandle) return;

	// Use a compact manual JSON build to avoid JSON library overhead on the hot path.
	FString Line;
	Line.Reserve(512);

	Line += FString::Printf(
		TEXT("{\"frame_id\":%llu,\"timestamp_us\":%llu,"),
		(unsigned long long)FrameIdx,
		(unsigned long long)Telemetry.TimestampUs);

	Line += FString::Printf(
		TEXT("\"platform\":{\"lat\":%.8f,\"lon\":%.8f,\"alt_m\":%.2f,")
		TEXT("\"yaw_deg\":%.4f,\"pitch_deg\":%.4f,\"roll_deg\":%.4f},"),
		Telemetry.Latitude, Telemetry.Longitude, Telemetry.Altitude,
		Telemetry.Yaw, Telemetry.Pitch, Telemetry.Roll);

	Line += TEXT("\"annotations\":[");
	bool bFirst = true;
	for (const FEntityAnnotationData& E : Entities)
	{
		// Caller (FGroundTruthCollector) already filtered to visible entities,
		// but guard here too for safety.
		if (!E.bVisible) continue;

		const float X = E.ScreenBBox.Min.X;
		const float Y = E.ScreenBBox.Min.Y;
		const float W = E.ScreenBBox.Max.X - E.ScreenBBox.Min.X;
		const float H = E.ScreenBBox.Max.Y - E.ScreenBBox.Min.Y;
		if (W <= 0.0f || H <= 0.0f) continue;

		if (!bFirst) Line += TEXT(",");
		bFirst = false;

		// Escape class name for JSON safety (replace " with \")
		FString SafeName = E.ClassName.Replace(TEXT("\""), TEXT("\\\""));

		Line += FString::Printf(
			TEXT("{\"entity_id\":%u,\"category\":{\"id\":%u,\"name\":\"%s\"},")
			TEXT("\"bbox\":[%.1f,%.1f,%.1f,%.1f],\"area\":%.1f,")
			TEXT("\"iscrowd\":0,\"truncated\":%d}"),
			E.EntityId,
			E.EntityType,
			*SafeName,
			X, Y, W, H,
			W * H,
			E.bTruncated ? 1 : 0);
	}
	Line += TEXT("]}\n");

	// Write as UTF-8 bytes directly to the persistent handle.
	FTCHARToUTF8 Utf8(*Line);
	if (!FileHandle->Write(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()))
	{
		UE_LOG(LogCamSim, Warning,
			TEXT("FCocoAnnotationWriter: write failed for frame %llu"), (unsigned long long)FrameIdx);
	}
}

void FCocoAnnotationWriter::Close()
{
	if (FileHandle)
	{
		delete FileHandle;
		FileHandle = nullptr;
	}
	bIsOpen = false;
}
