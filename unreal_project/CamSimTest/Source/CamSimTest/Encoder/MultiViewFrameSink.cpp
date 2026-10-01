// Copyright CamSim Contributors. All Rights Reserved.

#include "Encoder/MultiViewFrameSink.h"
#include "Encoder/Nv12.h"
#include "CamSimTest.h"

#include "Async/ParallelFor.h"

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

	bIsOpen = true;
	return true;
}

void FMultiViewFrameSink::EncodeFrame(const FSensorFrame& Frame,
                                      const FCamSimTelemetry& Telemetry,
                                      uint64 FrameIdx)
{
	if (!bIsOpen || Frame.IsEmpty()) return;

	const int32 Width = Config.CaptureWidth;
	const int32 Height = Config.CaptureHeight;
	const int32 ExpectedNum = CamSimNv12::NumBytes(Width, Height);
	const int32 ActualNum = Frame.Nv12.Num();
	if (ActualNum != ExpectedNum)
	{
		UE_LOG(LogCamSim, Warning,
			TEXT("FMultiViewFrameSink: unexpected pixel buffer size (%d, expected %d)"),
			ActualNum, ExpectedNum);
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

		const FSensorFrame* FrameForView = &Frame;
		// Phase 2: reuse the per-view ZoomedScratch buffer instead of
		// allocating a fresh TArray per view per frame.
		FSensorFrame& ZoomedFrame = View.ZoomedScratch;
		if (TargetHFov + KINDA_SMALL_NUMBER < SourceHFov)
		{
			ApplyDigitalZoomNv12(Frame.Nv12, Width, Height, SourceHFov, TargetHFov, ZoomedFrame.Nv12);
			FrameForView = &ZoomedFrame;
		}

		ViewTelemetry.HFovDeg = TargetHFov;
		ViewTelemetry.VFovDeg = TargetHFov * static_cast<float>(Height) / static_cast<float>(Width);

		View.Encoder->EncodeFrame(*FrameForView, ViewTelemetry, FrameIdx);
		++EncodedViews;
	}

	if (EncodedViews > 0)
	{
		++SuccessfulFrameCount;
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

void FMultiViewFrameSink::ApplyDigitalZoomNv12(const TArray<uint8>& SourceNv12,
                                           int32 Width, int32 Height,
                                           float SourceHFovDeg, float TargetHFovDeg,
                                           TArray<uint8>& OutNv12)
{
	OutNv12.SetNumUninitialized(SourceNv12.Num());
	if (TargetHFovDeg >= SourceHFovDeg || Width <= 1 || Height <= 1)
	{
		FMemory::Memcpy(OutNv12.GetData(), SourceNv12.GetData(), SourceNv12.Num());
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

	const uint8* SrcYPlane  = SourceNv12.GetData();
	const uint8* SrcUVPlane = SrcYPlane + Width * Height;
	uint8* DstYPlane  = OutNv12.GetData();
	uint8* DstUVPlane = DstYPlane + Width * Height;

	// Y plane: identical crop/nearest-neighbour mapping to ApplyDigitalZoom.
	ParallelFor(Height, [&](int32 Y)
	{
		const int32 SrcY = StartY + FMath::Clamp((Y * CropH) / Height, 0, CropH - 1);
		for (int32 X = 0; X < Width; ++X)
		{
			const int32 SrcX = StartX + FMath::Clamp((X * CropW) / Width, 0, CropW - 1);
			DstYPlane[Y * Width + X] = SrcYPlane[SrcY * Width + SrcX];
		}
	}, EParallelForFlags::BackgroundPriority);

	// UV plane: sample at (SrcY/2, (SrcX/2)*2), where SrcX/SrcY are the Y-plane
	// mapping evaluated at the even destination pixel each chroma pair covers.
	const int32 HalfW = Width / 2;
	const int32 HalfH = Height / 2;
	ParallelFor(HalfH, [&](int32 DstChromaY)
	{
		const int32 Y = DstChromaY * 2;
		const int32 SrcY = StartY + FMath::Clamp((Y * CropH) / Height, 0, CropH - 1);
		const int32 SrcChromaRow = SrcY / 2;
		for (int32 DstChromaX = 0; DstChromaX < HalfW; ++DstChromaX)
		{
			const int32 X = DstChromaX * 2;
			const int32 SrcX = StartX + FMath::Clamp((X * CropW) / Width, 0, CropW - 1);
			const int32 SrcChromaCol = SrcX / 2;
			const uint8* SrcPair = SrcUVPlane + SrcChromaRow * Width + SrcChromaCol * 2;
			uint8* DstPair = DstUVPlane + DstChromaY * Width + DstChromaX * 2;
			DstPair[0] = SrcPair[0];
			DstPair[1] = SrcPair[1];
		}
	}, EParallelForFlags::BackgroundPriority);
}
