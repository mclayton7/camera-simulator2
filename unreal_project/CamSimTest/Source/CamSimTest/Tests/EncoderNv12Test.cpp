// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Config/CamSimConfig.h"
#include "Encoder/VideoEncoder.h"
#include "Encoder/Nv12.h"

extern "C"
{
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
}

namespace
{
	/** Decode the last video frame of a .ts file; returns its Y plane (W*H) or empty. */
	TArray<uint8> DecodeLastLuma(const FString& Path, int32 W, int32 H)
	{
		TArray<uint8> Y;
		AVFormatContext* Fmt = nullptr;
		if (avformat_open_input(&Fmt, TCHAR_TO_UTF8(*Path), nullptr, nullptr) < 0) return Y;
		avformat_find_stream_info(Fmt, nullptr);
		const int Stream = av_find_best_stream(Fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		const AVCodec* Codec = avcodec_find_decoder(Fmt->streams[Stream]->codecpar->codec_id);
		AVCodecContext* Ctx = avcodec_alloc_context3(Codec);
		avcodec_parameters_to_context(Ctx, Fmt->streams[Stream]->codecpar);
		avcodec_open2(Ctx, Codec, nullptr);
		AVPacket* Pkt = av_packet_alloc();
		AVFrame* Frame = av_frame_alloc();
		auto Drain = [&]()
		{
			while (avcodec_receive_frame(Ctx, Frame) == 0 && Frame->width == W && Frame->height == H)
			{
				Y.SetNumUninitialized(W * H);
				for (int32 R = 0; R < H; ++R) FMemory::Memcpy(Y.GetData() + R * W, Frame->data[0] + R * Frame->linesize[0], W);
			}
		};
		while (av_read_frame(Fmt, Pkt) >= 0)
		{
			if (Pkt->stream_index == Stream && avcodec_send_packet(Ctx, Pkt) == 0) Drain();
			av_packet_unref(Pkt);
		}
		avcodec_send_packet(Ctx, nullptr);
		Drain();
		av_frame_free(&Frame); av_packet_free(&Pkt); avcodec_free_context(&Ctx); avformat_close_input(&Fmt);
		return Y;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEncoderNv12RoundTripTest, "CamSim.Encoder.Nv12.RoundTripPsnr",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FEncoderNv12RoundTripTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 640, H = 360, Frames = 30;
	FCamSimConfig Config;
	Config.CaptureWidth = W; Config.CaptureHeight = H; Config.FrameRate = 30.0f;
	Config.VideoBitrate = 8'000'000;
	Config.Encoder = TEXT("libx264");
	Config.EncoderPref = FCamSimConfig::EEncoderPreference::LibX264;
	Config.MulticastAddr = TEXT("127.0.0.1"); Config.MulticastPort = 49998;
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("nv12_roundtrip.ts"));
	IFileManager::Get().Delete(*Path);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	Config.Recording.VideoRecordPath = Path;

	FSensorFrame F;
	F.Format = ESensorPixelFormat::NV12;
	F.Nv12.SetNumUninitialized(CamSimNv12::NumBytes(W, H));
	for (int32 R = 0; R < H; ++R) for (int32 X = 0; X < W; ++X) F.Nv12[R * W + X] = static_cast<uint8>(16 + ((X + R) * 219) / (W + H));
	for (int32 I = W * H; I < F.Nv12.Num(); ++I) F.Nv12[I] = (I & 1) ? 150 : 110;
	{
		FVideoEncoder Encoder(Config);
		if (!TestTrue(TEXT("opened"), Encoder.Open())) return false;
		FCamSimTelemetry T;
		for (int32 I = 0; I < Frames; ++I) Encoder.EncodeFrame(F, T, I);
		Encoder.Close();
	}
	const TArray<uint8> Y = DecodeLastLuma(Path, W, H);
	IFileManager::Get().Delete(*Path);
	if (!TestEqual(TEXT("decoded a frame"), Y.Num(), W * H)) return false;
	double Mse = 0.0;
	for (int32 I = 0; I < W * H; ++I) { const double D = double(Y[I]) - double(F.Nv12[I]); Mse += D * D; }
	Mse /= (W * H);
	const double Psnr = Mse > 0.0 ? 10.0 * FMath::LogX(10.0, 255.0 * 255.0 / Mse) : 99.0;
	TestTrue(FString::Printf(TEXT("luma PSNR %.1f dB >= 40"), Psnr), Psnr >= 40.0);
	return true;
}
