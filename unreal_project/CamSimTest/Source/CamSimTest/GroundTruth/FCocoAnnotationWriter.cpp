// Copyright CamSim Contributors. All Rights Reserved.

#include "GroundTruth/FCocoAnnotationWriter.h"
#include "Metadata/KlvBuilder.h"  // FCamSimTelemetry
#include "CamSimTest.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

namespace
{
	/** JSON string-body escape: backslash and quote (RLE counts contain backslashes). */
	FString JsonEscape(const FString& In)
	{
		return In.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
	}
}

bool FCocoAnnotationWriter::Open(const FString& OutputDir)
{
	if (bIsOpen) return true;

	IFileManager::Get().MakeDirectory(*OutputDir, /*Tree=*/true);

	JsonlPath = FPaths::Combine(OutputDir, TEXT("camsim_coco.jsonl"));

	// Create / truncate the file and keep a persistent append handle.
	// Using a persistent IFileHandle avoids per-frame open+close+seek overhead
	// that would otherwise accumulate on multi-hour capture sessions.
	FileHandle = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*JsonlPath, /*bAppend=*/false, /*bAllowRead=*/true);
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

		const FString SafeName = JsonEscape(E.ClassName);
		const FString SafeSource = JsonEscape(E.Source);
		const FString SafeSourceId = JsonEscape(E.SourceId);

		Line += FString::Printf(
			TEXT("{\"entity_id\":%u,\"source\":\"%s\",\"source_id\":\"%s\",\"category\":{\"id\":%u,\"name\":\"%s\"},")
			TEXT("\"bbox\":[%.1f,%.1f,%.1f,%.1f],\"area\":%.1f,")
			TEXT("\"iscrowd\":0,\"truncated\":%d"),
			E.EntityId, *SafeSource, *SafeSourceId,
			E.EntityType,
			*SafeName,
			X, Y, W, H,
			E.bMaskMeasured ? double(E.VisiblePixels) : double(W) * H,
			E.bTruncated ? 1 : 0);
		if (E.bMaskMeasured)
		{
			// bbox/area above already come from the mask (ScreenBBox = modal box; area = VisiblePixels, not W*H)
			Line += FString::Printf(TEXT(",\"mask_source\":\"render\",\"visibility\":%.4f"),
				E.AmodalPixels > 0 ? double(E.VisiblePixels) / E.AmodalPixels : 0.0);
			Line += FString::Printf(TEXT(",\"bbox_amodal\":[%.1f,%.1f,%.1f,%.1f]"), E.AmodalBBox.Min.X, E.AmodalBBox.Min.Y,
				E.AmodalBBox.Max.X - E.AmodalBBox.Min.X, E.AmodalBBox.Max.Y - E.AmodalBBox.Min.Y);
			auto Obb = [](const CamSimMask::FOrientedBox& B) { return FString::Printf(TEXT("[%.2f,%.2f,%.2f,%.2f,%.2f]"), B.Cx, B.Cy, B.W, B.H, B.AngleDeg); };
			Line += TEXT(",\"obb\":") + Obb(E.Obb) + TEXT(",\"obb_amodal\":") + Obb(E.ObbAmodal);
			if (!E.SegmentationRle.IsEmpty())
				Line += FString::Printf(TEXT(",\"segmentation\":{\"size\":[%d,%d],\"counts\":\"%s\"}"), ImageHeight, ImageWidth, *JsonEscape(E.SegmentationRle));
		}
		else
		{
			Line += TEXT(",\"mask_source\":\"projection\"");
		}
		if (E.Truncation >= 0.0) Line += FString::Printf(TEXT(",\"truncation\":%.4f"), E.Truncation);
		if (E.bHasBox3D)
		{
			Line += FString::Printf(TEXT(",\"box3d\":{\"size_m\":[%.3f,%.3f,%.3f],\"yaw_deg\":%.3f,\"pitch_deg\":%.3f,\"roll_deg\":%.3f,\"corners_px\":"),
				E.Box3DSizeM.X, E.Box3DSizeM.Y, E.Box3DSizeM.Z, E.YawDeg, E.PitchDeg, E.RollDeg);
			if (E.bCornersValid)
			{
				Line += TEXT("[");
				for (int32 C = 0; C < 8; ++C) Line += FString::Printf(TEXT("%s[%.1f,%.1f]"), C ? TEXT(",") : TEXT(""), E.CornersPx[C].X, E.CornersPx[C].Y);
				Line += TEXT("]}");
			}
			else
			{
				Line += TEXT("null}");
			}
		}
		if (E.bHasGeo)
		{
			Line += FString::Printf(TEXT(",\"geo\":{\"lat\":%.8f,\"lon\":%.8f,\"alt_m\":%.3f}"), E.Lat, E.Lon, E.AltM);
		}
		Line += TEXT("}");
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
