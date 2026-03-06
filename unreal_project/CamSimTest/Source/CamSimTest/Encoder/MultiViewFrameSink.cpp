// Copyright CamSim Contributors. All Rights Reserved.

#include "Encoder/MultiViewFrameSink.h"
#include "CamSimTest.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

FMultiViewFrameSink::FMultiViewFrameSink(const FCamSimConfig& InConfig)
	: Config(InConfig)
{
}

FMultiViewFrameSink::~FMultiViewFrameSink()  // NOLINT(modernize-use-override)
{
	if (bIsOpen)
	{
		Close();
	}
}

bool FMultiViewFrameSink::Open()
{
	if (bIsOpen) return true;

	BuildViewRuntimes();
	int32 OpenCount = 0;
	for (FViewRuntime& View : Views)
	{
		View.Encoder = MakeUnique<FVideoEncoder>(View.ViewConfig);
		if (View.Encoder->Open())
		{
			++OpenCount;
			UE_LOG(LogCamSim, Log,
				TEXT("FMultiViewFrameSink: opened view=%d route=%s hfov=%.2f"),
				View.ViewId, *View.RouteLabel, View.OutputHFovDeg);
		}
		else
		{
			UE_LOG(LogCamSim, Error,
				TEXT("FMultiViewFrameSink: failed to open view=%d route=%s"),
				View.ViewId, *View.RouteLabel);
			View.Encoder.Reset();
		}
	}

	if (OpenCount == 0)
	{
		UE_LOG(LogCamSim, Error, TEXT("FMultiViewFrameSink: no output views opened"));
		return false;
	}

	bGroundTruthEnabled = Config.GroundTruth.bEnabled;
	GroundTruthIntervalFrames = FMath::Max(1, Config.GroundTruth.IntervalFrames);
	GroundTruthPath = Config.GroundTruth.OutputPath;
	if (GroundTruthPath.IsEmpty())
	{
		GroundTruthPath = FPaths::Combine(FPlatformProcess::BaseDir(), TEXT("camsim_groundtruth.jsonl"));
	}
	if (FPaths::IsRelative(GroundTruthPath))
	{
		GroundTruthPath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPlatformProcess::BaseDir(), GroundTruthPath));
	}
	if (bGroundTruthEnabled)
	{
		FFileHelper::SaveStringToFile(TEXT(""), *GroundTruthPath);
		UE_LOG(LogCamSim, Log, TEXT("FMultiViewFrameSink: ground-truth sidecar enabled -> %s"), *GroundTruthPath);
	}

	bIsOpen = true;
	return true;
}

void FMultiViewFrameSink::EncodeFrame(const TArray<FColor>& PixelData,
                                      const FCamSimTelemetry& Telemetry,
                                      uint64 FrameIdx)
{
	if (!bIsOpen || PixelData.Num() == 0) return;

	const int32 Width = Config.CaptureWidth;
	const int32 Height = Config.CaptureHeight;
	if (PixelData.Num() != Width * Height)
	{
		UE_LOG(LogCamSim, Warning,
			TEXT("FMultiViewFrameSink: unexpected pixel buffer size (%d, expected %d)"),
			PixelData.Num(), Width * Height);
		return;
	}

	int32 EncodedViews = 0;
	for (FViewRuntime& View : Views)
	{
		if (!View.Encoder || !View.Encoder->IsOpen()) continue;

		FCamSimTelemetry ViewTelemetry = Telemetry;
		const float SourceHFov = FMath::Max(0.1f, Telemetry.HFovDeg);
		const float TargetHFov = (View.OutputHFovDeg > 0.0f)
			? FMath::Clamp(View.OutputHFovDeg, 1.0f, SourceHFov)
			: SourceHFov;

		const TArray<FColor>* PixelsForView = &PixelData;
		TArray<FColor> ZoomedPixels;
		if (TargetHFov + KINDA_SMALL_NUMBER < SourceHFov)
		{
			ApplyDigitalZoom(PixelData, Width, Height, SourceHFov, TargetHFov, ZoomedPixels);
			PixelsForView = &ZoomedPixels;
		}

		ViewTelemetry.HFovDeg = TargetHFov;
		ViewTelemetry.VFovDeg = TargetHFov * static_cast<float>(Height) / static_cast<float>(Width);

		View.Encoder->EncodeFrame(*PixelsForView, ViewTelemetry, FrameIdx);
		++EncodedViews;
	}

	if (EncodedViews > 0)
	{
		++SuccessfulFrameCount;
	}

	if (bGroundTruthEnabled && ((FrameIdx % static_cast<uint64>(GroundTruthIntervalFrames)) == 0))
	{
		WriteGroundTruthLine(Telemetry, FrameIdx, EncodedViews);
	}
}

void FMultiViewFrameSink::Close()
{
	if (!bIsOpen) return;
	bIsOpen = false;

	for (FViewRuntime& View : Views)
	{
		if (View.Encoder)
		{
			View.Encoder->Close();
			View.Encoder.Reset();
		}
	}
	Views.Reset();
}

void FMultiViewFrameSink::BuildViewRuntimes()
{
	Views.Reset();
	auto AddRuntime = [&](const FCamSimConfig::FOutputViewConfig& ViewCfg)
	{
		FViewRuntime Runtime;
		Runtime.ViewId = ViewCfg.ViewId;
		Runtime.ViewConfig = Config;
		Runtime.ViewConfig.MulticastAddr = ViewCfg.MulticastAddr;
		Runtime.ViewConfig.MulticastPort = ViewCfg.MulticastPort;
		Runtime.ViewConfig.VideoBitrate = ViewCfg.VideoBitrate;
		Runtime.ViewConfig.H264Preset = ViewCfg.H264Preset;
		Runtime.ViewConfig.H264Tune = ViewCfg.H264Tune;
		Runtime.OutputHFovDeg = ViewCfg.HFovDeg;
		Runtime.RouteLabel = FString::Printf(TEXT("udp://%s:%d"),
			*Runtime.ViewConfig.MulticastAddr, Runtime.ViewConfig.MulticastPort);
		Views.Add(MoveTemp(Runtime));
	};

	if (Config.OutputViews.Num() > 0)
	{
		for (const FCamSimConfig::FOutputViewConfig& ViewCfg : Config.OutputViews)
		{
			if (!ViewCfg.bEnabled) continue;
			AddRuntime(ViewCfg);
		}
	}

	if (Views.Num() == 0)
	{
		FCamSimConfig::FOutputViewConfig DefaultView;
		DefaultView.ViewId = 0;
		DefaultView.bEnabled = true;
		DefaultView.MulticastAddr = Config.MulticastAddr;
		DefaultView.MulticastPort = Config.MulticastPort;
		DefaultView.VideoBitrate = Config.VideoBitrate;
		DefaultView.H264Preset = Config.H264Preset;
		DefaultView.H264Tune = Config.H264Tune;
		DefaultView.HFovDeg = 0.0f;
		AddRuntime(DefaultView);
	}
}

void FMultiViewFrameSink::WriteGroundTruthLine(const FCamSimTelemetry& Telemetry,
                                               uint64 FrameIdx,
                                               int32 EncodedViewCount) const
{
	if (GroundTruthPath.IsEmpty()) return;

	FString ViewsJson;
	ViewsJson += TEXT("[");
	for (int32 i = 0; i < Views.Num(); ++i)
	{
		ViewsJson += FString::Printf(TEXT("{\"view_id\":%d,\"route\":\"%s\"}"),
			Views[i].ViewId, *Views[i].RouteLabel);
		if (i + 1 < Views.Num()) ViewsJson += TEXT(",");
	}
	ViewsJson += TEXT("]");

	const FString Line = FString::Printf(
		TEXT("{\"frame\":%llu,\"timestamp_us\":%llu,\"lat\":%.8f,\"lon\":%.8f,\"alt_m\":%.3f,")
		TEXT("\"yaw_deg\":%.3f,\"pitch_deg\":%.3f,\"roll_deg\":%.3f,")
		TEXT("\"hfov_deg\":%.3f,\"vfov_deg\":%.3f,")
		TEXT("\"gimbal_yaw_deg\":%.3f,\"gimbal_pitch_deg\":%.3f,\"gimbal_roll_deg\":%.3f,")
		TEXT("\"slant_range_m\":%.3f,\"frame_center_lat\":%.8f,\"frame_center_lon\":%.8f,")
		TEXT("\"sensor_mode\":%u,\"sensor_polarity\":%u,")
		TEXT("\"encoded_views\":%d,\"configured_views\":%d,\"views\":%s}\n"),
		FrameIdx, Telemetry.TimestampUs,
		Telemetry.Latitude, Telemetry.Longitude, Telemetry.Altitude,
		Telemetry.Yaw, Telemetry.Pitch, Telemetry.Roll,
		Telemetry.HFovDeg, Telemetry.VFovDeg,
		Telemetry.GimbalYaw, Telemetry.GimbalPitch, Telemetry.GimbalRoll,
		Telemetry.SlantRangeM, Telemetry.FrameCenterLat, Telemetry.FrameCenterLon,
		static_cast<unsigned>(Telemetry.SensorMode),
		static_cast<unsigned>(Telemetry.SensorPolarity),
		EncodedViewCount, Views.Num(), *ViewsJson);

	FFileHelper::SaveStringToFile(
		Line, *GroundTruthPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
		&IFileManager::Get(),
		FILEWRITE_Append | FILEWRITE_AllowRead);
}

void FMultiViewFrameSink::ApplyDigitalZoom(const TArray<FColor>& SourcePixels,
                                           int32 Width, int32 Height,
                                           float SourceHFovDeg, float TargetHFovDeg,
                                           TArray<FColor>& OutPixels)
{
	OutPixels.SetNumUninitialized(SourcePixels.Num());
	if (TargetHFovDeg >= SourceHFovDeg || Width <= 1 || Height <= 1)
	{
		FMemory::Memcpy(OutPixels.GetData(), SourcePixels.GetData(), SourcePixels.Num() * sizeof(FColor));
		return;
	}

	const float SrcHalf = FMath::DegreesToRadians(SourceHFovDeg * 0.5f);
	const float DstHalf = FMath::DegreesToRadians(TargetHFovDeg * 0.5f);
	const float Zoom = FMath::Tan(SrcHalf) / FMath::Max(KINDA_SMALL_NUMBER, FMath::Tan(DstHalf));
	const float CropFactor = FMath::Clamp(1.0f / Zoom, 0.05f, 1.0f);

	const int32 CropW = FMath::Clamp(FMath::RoundToInt(Width * CropFactor), 1, Width);
	const int32 CropH = FMath::Clamp(FMath::RoundToInt(Height * CropFactor), 1, Height);
	const int32 StartX = (Width - CropW) / 2;
	const int32 StartY = (Height - CropH) / 2;

	for (int32 Y = 0; Y < Height; ++Y)
	{
		const int32 SrcY = StartY + FMath::Clamp((Y * CropH) / Height, 0, CropH - 1);
		for (int32 X = 0; X < Width; ++X)
		{
			const int32 SrcX = StartX + FMath::Clamp((X * CropW) / Width, 0, CropW - 1);
			OutPixels[Y * Width + X] = SourcePixels[SrcY * Width + SrcX];
		}
	}
}
