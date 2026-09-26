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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVideoEncoderVbvCapTest,
	"CamSim.VideoEncoder.VbvCapsBurstsOnNoise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVideoEncoderVbvCapTest::RunTest(const FString& Parameters)
{
	const FString RecordPath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("Automation/vbv_cap_test.ts"));
	IFileManager::Get().Delete(*RecordPath);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(RecordPath), true);

	FCamSimConfig Config;
	Config.CaptureWidth  = 640;
	Config.CaptureHeight = 360;
	Config.FrameRate     = 30.0f;
	Config.VideoBitrate  = 500'000;
	Config.Encoder       = TEXT("libx264");
	Config.EncoderPref   = FCamSimConfig::EEncoderPreference::LibX264;
	Config.MulticastAddr = TEXT("127.0.0.1");
	Config.MulticastPort = 49999;  // nothing listens; UDP send is fire-and-forget
	Config.Recording.VideoRecordPath = RecordPath;

	constexpr int32 NumFrames = 90;
	{
		FVideoEncoder Encoder(Config);
		if (!TestTrue(TEXT("Encoder opened"), Encoder.Open()))
		{
			return false;
		}
		FRandomStream Rng(1234);
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Config.CaptureWidth * Config.CaptureHeight);
		FCamSimTelemetry T;
		for (int32 Frame = 0; Frame < NumFrames; ++Frame)
		{
			for (FColor& C : Pixels)
			{
				C = FColor(Rng.RandRange(0, 255), Rng.RandRange(0, 255), Rng.RandRange(0, 255), 255);
			}
			Encoder.EncodeFrame(Pixels, T, Frame);
		}
		Encoder.Close();
	}

	TArray<uint8> Ts;
	if (!TestTrue(TEXT("Recording written"), FFileHelper::LoadFileToArray(Ts, *RecordPath)))
	{
		return false;
	}
	const TArray<int64> Sizes = ReadVideoFrameSizes(Ts);
	TestTrue(FString::Printf(TEXT("Found video frames (%d)"), Sizes.Num()), Sizes.Num() >= NumFrames - 5);

	const double BytesPerSec = Config.VideoBitrate / 8.0;
	const double Tolerance   = 1.10;  // PES/NAL framing overhead
	for (const int32 Window : { 1, 15, 30, 60 })
	{
		int64 Worst = 0;
		for (int32 Start = 0; Start + Window <= Sizes.Num(); ++Start)
		{
			int64 Sum = 0;
			for (int32 i = Start; i < Start + Window; ++i) Sum += Sizes[i];
			Worst = FMath::Max(Worst, Sum);
		}
		const double Limit = BytesPerSec * (Window / Config.FrameRate + FVideoEncoder::VbvBufferSec) * Tolerance;
		TestTrue(FString::Printf(TEXT("Worst %d-frame window %lld B <= %.0f B"), Window, Worst, Limit),
			Worst <= Limit);
	}

	IFileManager::Get().Delete(*RecordPath);
	return true;
}
