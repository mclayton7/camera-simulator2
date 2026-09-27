// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Math/RandomStream.h"
#include "Encoder/VideoEncoder.h"
#include "Config/CamSimConfig.h"
#include "Metadata/CamSimTelemetry.h"

// -------------------------------------------------------------------------
// libx264 must respect the VBV cap: incompressible input (random noise) is
// the worst case for bursts. Over any window of N frames the video payload
// must stay within bitrate * (N / fps + VbvBufferSec).
// -------------------------------------------------------------------------

namespace
{
	/** Sum of video PES payload bytes per access unit, from an MPEG-TS file. */
	TArray<int64> ReadVideoFrameSizes(const TArray<uint8>& Ts)
	{
		// PAT -> PMT -> first video PID (stream_type 0x1B H.264 / 0x24 HEVC)
		int32 PmtPid = -1, VideoPid = -1;
		for (int32 Off = 0; Off + 188 <= Ts.Num() && VideoPid < 0; Off += 188)
		{
			const uint8* P = &Ts[Off];
			if (P[0] != 0x47) continue;
			const int32 Pid  = ((P[1] & 0x1F) << 8) | P[2];
			const bool  Pusi = (P[1] & 0x40) != 0;
			int32 Pos = 4;
			if (P[3] & 0x20) Pos += 1 + P[4];  // adaptation field
			if (!Pusi || Pos >= 188) continue;
			Pos += 1 + P[Pos];                  // pointer field
			if (Pid == 0 && PmtPid < 0)
			{
				PmtPid = ((P[Pos + 10] & 0x1F) << 8) | P[Pos + 11];
			}
			else if (Pid == PmtPid)
			{
				const int32 SectionLen = ((P[Pos + 1] & 0x0F) << 8) | P[Pos + 2];
				const int32 ProgInfoLen = ((P[Pos + 10] & 0x0F) << 8) | P[Pos + 11];
				int32 Es = Pos + 12 + ProgInfoLen;
				const int32 EsEnd = Pos + 3 + SectionLen - 4;
				for (; Es + 5 <= EsEnd && Es + 5 <= 188; )
				{
					const uint8 Type = P[Es];
					const int32 EsPid = ((P[Es + 1] & 0x1F) << 8) | P[Es + 2];
					const int32 EsInfo = ((P[Es + 3] & 0x0F) << 8) | P[Es + 4];
					if (Type == 0x1B || Type == 0x24) { VideoPid = EsPid; break; }
					Es += 5 + EsInfo;
				}
			}
		}

		TArray<int64> Sizes;
		if (VideoPid < 0) return Sizes;
		for (int32 Off = 0; Off + 188 <= Ts.Num(); Off += 188)
		{
			const uint8* P = &Ts[Off];
			if (P[0] != 0x47 || (((P[1] & 0x1F) << 8) | P[2]) != VideoPid || !(P[3] & 0x10)) continue;
			int32 Pos = 4;
			if (P[3] & 0x20) Pos += 1 + P[4];
			if (Pos >= 188) continue;
			if (P[1] & 0x40)
			{
				Sizes.Add(0);
				Pos += 9 + P[Pos + 8];  // skip PES header
			}
			if (Sizes.Num() > 0 && Pos < 188) Sizes.Last() += 188 - Pos;
		}
		return Sizes;
	}

	/**
	 * Encodes NumFrames to a .ts recording; returns per-AU sizes. GrainAmp >= 255
	 * is pure random noise (worst case); smaller values are a scrolling gradient
	 * with +/-GrainAmp of grain, a stand-in for a real sensor scene.
	 */
	bool EncodeNoise(FAutomationTestBase& Test, FCamSimConfig& Config, const TCHAR* Name,
		int32 NumFrames, TArray<int64>& OutSizes,
		int32 Width = 640, int32 Height = 360, int32 Bitrate = 500'000, int32 GrainAmp = 255)
	{
		const FString RecordPath = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("Automation") / Name);
		IFileManager::Get().Delete(*RecordPath);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(RecordPath), true);

		Config.CaptureWidth  = Width;
		Config.CaptureHeight = Height;
		Config.FrameRate     = 30.0f;
		Config.VideoBitrate  = Bitrate;
		Config.MulticastAddr = TEXT("127.0.0.1");
		Config.MulticastPort = 49999;  // nothing listens; UDP send is fire-and-forget
		Config.Recording.VideoRecordPath = RecordPath;
		{
			FVideoEncoder Encoder(Config, ESensorPipelinePath::Legacy);
			if (!Test.TestTrue(TEXT("Encoder opened"), Encoder.Open()))
			{
				return false;
			}
			FRandomStream Rng(1234);
			FSensorFrame Frame;
			Frame.Format = ESensorPixelFormat::BGRA8;
			TArray<FColor>& Pixels = Frame.Bgra;
			Pixels.SetNumUninitialized(Config.CaptureWidth * Config.CaptureHeight);
			FCamSimTelemetry T;
			for (int32 FrameI = 0; FrameI < NumFrames; ++FrameI)
			{
				for (int32 i = 0; i < Pixels.Num(); ++i)
				{
					FColor& C = Pixels[i];
					if (GrainAmp >= 255)
					{
						C = FColor(Rng.RandRange(0, 255), Rng.RandRange(0, 255), Rng.RandRange(0, 255), 255);
						continue;
					}
					const int32 X = i % Width, Y = i / Width;
					const int32 Base = (X + Y + FrameI * 8) & 255;
					const uint8 V = (uint8)FMath::Clamp(Base + Rng.RandRange(-GrainAmp, GrainAmp), 0, 255);
					C = FColor(V, V, V, 255);
				}
				Encoder.EncodeFrame(Frame, T, FrameI);
			}
			Encoder.Close();
		}

		TArray<uint8> Ts;
		if (!Test.TestTrue(TEXT("Recording written"), FFileHelper::LoadFileToArray(Ts, *RecordPath)))
		{
			return false;
		}
		IFileManager::Get().Delete(*RecordPath);
		OutSizes = ReadVideoFrameSizes(Ts);
		return Test.TestTrue(FString::Printf(TEXT("Found video frames (%d)"), OutSizes.Num()),
			OutSizes.Num() >= NumFrames - 5);
	}

	/** Largest total payload over any Window consecutive frames. */
	int64 WorstWindow(const TArray<int64>& Sizes, int32 Window)
	{
		int64 Worst = 0;
		for (int32 Start = 0; Start + Window <= Sizes.Num(); ++Start)
		{
			int64 Sum = 0;
			for (int32 i = Start; i < Start + Window; ++i) Sum += Sizes[i];
			Worst = FMath::Max(Worst, Sum);
		}
		return Worst;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVideoEncoderVbvCapTest,
	"CamSim.VideoEncoder.VbvCapsBurstsOnNoise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVideoEncoderVbvCapTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Config;
	Config.Encoder     = TEXT("libx264");
	Config.EncoderPref = FCamSimConfig::EEncoderPreference::LibX264;

	constexpr int32 NumFrames = 90;
	TArray<int64> Sizes;
	if (!EncodeNoise(*this, Config, TEXT("vbv_cap_test.ts"), NumFrames, Sizes))
	{
		return false;
	}

	const double BytesPerSec = Config.VideoBitrate / 8.0;
	const double Tolerance   = 1.10;  // PES/NAL framing overhead
	for (const int32 Window : { 1, 15, 30, 60 })
	{
		const int64 Worst = WorstWindow(Sizes, Window);
		const double Limit = BytesPerSec * (Window / Config.FrameRate + FVideoEncoder::VbvBufferSec) * Tolerance;
		TestTrue(FString::Printf(TEXT("Worst %d-frame window %lld B <= %.0f B"), Window, Worst, Limit),
			Worst <= Limit);
	}
	return true;
}

// -------------------------------------------------------------------------
// VideoToolbox (macOS hardware encoder): opens when requested explicitly and
// honours rc_max_rate, which it applies as a per-second data-rate limit.
// Uses a grainy scrolling gradient at the shipped default (1280x720, 4 Mbps).
// Pure noise overshoots the cap ~3.6x even at QP 51 / constant_bit_rate —
// the reason Auto doesn't pick VideoToolbox. Skipped on builds without
// VideoToolbox (Linux).
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVideoEncoderVideoToolboxTest,
	"CamSim.VideoEncoder.VideoToolboxRespectsMaxRate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVideoEncoderVideoToolboxTest::RunTest(const FString& Parameters)
{
	if (!avcodec_find_encoder_by_name("h264_videotoolbox"))
	{
		AddInfo(TEXT("h264_videotoolbox not built in; skipping"));
		return true;
	}

	FCamSimConfig Config;
	Config.Encoder     = TEXT("videotoolbox");
	Config.EncoderPref = FCamSimConfig::EEncoderPreference::VideoToolbox;

	constexpr int32 NumFrames = 90;
	TArray<int64> Sizes;
	if (!EncodeNoise(*this, Config, TEXT("videotoolbox_test.ts"), NumFrames, Sizes,
		1280, 720, 4'000'000, /*GrainAmp=*/12))
	{
		return false;
	}

	const double BytesPerSec = Config.VideoBitrate / 8.0;
	const double Tolerance   = 1.10;  // PES/NAL framing overhead
	const int32  OneSecond   = FMath::RoundToInt(Config.FrameRate);
	const int64  Worst       = WorstWindow(Sizes, OneSecond);
	const double Limit       = BytesPerSec * Tolerance;
	TestTrue(FString::Printf(TEXT("Worst 1 s window %lld B <= %.0f B"), Worst, Limit), Worst <= Limit);
	return true;
}
