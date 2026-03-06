// Copyright CamSim Contributors. All Rights Reserved.

#include "Subsystem/CamSimSubsystem.h"
#include "Entity/CamSimEntityManager.h"
#include "CIGI/CigiReceiver.h"
#include "CIGI/CigiSender.h"
#include "CIGI/CigiQueryHandler.h"
#include "Encoder/VideoEncoder.h"   // FVideoEncoder (concrete IFrameSink)
#include "Encoder/IFrameSink.h"
#include "CamSimTest.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "DynamicRHI.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

extern "C"
{
#include "libavcodec/version.h"
#include "libswscale/version.h"
}

namespace
{
const TCHAR* ReadbackFormatToString(FCamSimConfig::EReadbackFormat Fmt)
{
	switch (Fmt)
	{
		case FCamSimConfig::EReadbackFormat::BGRA: return TEXT("bgra");
		case FCamSimConfig::EReadbackFormat::RGBA: return TEXT("rgba");
		case FCamSimConfig::EReadbackFormat::ARGB: return TEXT("argb");
		case FCamSimConfig::EReadbackFormat::ABGR: return TEXT("abgr");
		case FCamSimConfig::EReadbackFormat::Auto:
		default: return TEXT("auto");
	}
}

const TCHAR* WatchdogPolicyToString(FCamSimConfig::EEncoderWatchdogPolicy Policy)
{
	switch (Policy)
	{
		case FCamSimConfig::EEncoderWatchdogPolicy::LogOnly: return TEXT("log_only");
		case FCamSimConfig::EEncoderWatchdogPolicy::FailFast: return TEXT("fail_fast");
		case FCamSimConfig::EEncoderWatchdogPolicy::Reconnect:
		default: return TEXT("reconnect");
	}
}
}

void UCamSimSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Load config — also return the parsed JSON root so we can load entity types
	// from the same document without re-reading the file.
	TSharedPtr<FJsonObject> JsonRoot;
	Config = FCamSimConfig::Load(&JsonRoot);
	if (JsonRoot.IsValid())
	{
		EntityTypeTable.LoadFromConfig(JsonRoot);
	}

	const FString RHIName = GDynamicRHI ? GDynamicRHI->GetName() : TEXT("Unknown");
	UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: initializing"));
	UE_LOG(LogCamSim, Log,
		TEXT("CamSim startup diagnostics: platform=%s rhi=%s ffmpeg(libavcodec=%d.%d.%d libswscale=%d.%d.%d) ")
		TEXT("video=udp://%s:%d cigi_in=%s:%d cigi_resp=%s:%d capture=%dx%d@%.1ffps ")
		TEXT("readback=%s swap_rb=%d ready_polls=%d bitrate=%d preset=%s tune=%s watchdog_policy=%s watchdog_ticks=%d"),
		ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName()), *RHIName,
		LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR, LIBAVCODEC_VERSION_MICRO,
		LIBSWSCALE_VERSION_MAJOR, LIBSWSCALE_VERSION_MINOR, LIBSWSCALE_VERSION_MICRO,
		*Config.MulticastAddr, Config.MulticastPort,
		*Config.CigiBindAddr, Config.CigiPort,
		*Config.CigiResponseAddr, Config.CigiResponsePort,
		Config.CaptureWidth, Config.CaptureHeight, Config.FrameRate,
		ReadbackFormatToString(Config.ReadbackFormat), Config.bSwapRBReadback ? 1 : 0,
		Config.ReadbackReadyPolls,
		Config.VideoBitrate, *Config.H264Preset, *Config.H264Tune,
		WatchdogPolicyToString(Config.EncoderWatchdogPolicy), Config.EncoderWatchdogIntervalTicks);

	// Start CIGI receiver thread
	CigiReceiver = new FCigiReceiver(Config);
	if (!CigiReceiver->Start())
	{
		UE_LOG(LogCamSim, Error, TEXT("UCamSimSubsystem: failed to start CIGI receiver"));
	}

	// Start FFmpeg encoder / MPEG-TS muxer
	VideoEncoder = new FVideoEncoder(Config);
	if (!VideoEncoder->Open())
	{
		UE_LOG(LogCamSim, Error, TEXT("UCamSimSubsystem: failed to open video encoder"));
	}

	// Create entity manager (game thread tickable — no explicit start needed)
	EntityManager = new FCamSimEntityManager(this, &EntityTypeTable);
	UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: entity manager created"));

	// Start CIGI sender (IG → host: SOF heartbeat + HAT/HOT + LOS responses)
	CigiSender = new FCigiSender();
	if (!CigiSender->Open(Config))
	{
		UE_LOG(LogCamSim, Warning, TEXT("UCamSimSubsystem: CIGI sender failed to open (responses disabled)"));
	}

	// Create query handler (drains HAT/HOT + LOS queues, runs line traces)
	QueryHandler = new FCigiQueryHandler(this, CigiSender);
	UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: CIGI query handler created"));
}

void UCamSimSubsystem::Deinitialize()
{
	UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: shutting down"));

	if (QueryHandler)
	{
		delete QueryHandler;
		QueryHandler = nullptr;
	}

	if (CigiSender)
	{
		CigiSender->Close();
		delete CigiSender;
		CigiSender = nullptr;
	}

	if (VideoEncoder)
	{
		VideoEncoder->Close();
		delete VideoEncoder;
		VideoEncoder = nullptr;
	}

	if (EntityManager)
	{
		delete EntityManager;
		EntityManager = nullptr;
	}

	if (CigiReceiver)
	{
		CigiReceiver->Stop();
		delete CigiReceiver;
		CigiReceiver = nullptr;
	}

	Super::Deinitialize();
}

void UCamSimSubsystem::Tick(float DeltaTime)
{
	// Drain HAT/HOT + LOS query queues and stage responses
	if (QueryHandler)
	{
		QueryHandler->Tick(DeltaTime);
	}

	// Flush SOF + all staged responses into one UDP datagram
	if (CigiSender)
	{
		CigiSender->FlushFrame(FrameCntr, 0);
	}

	// -----------------------------------------------------------------------
	// Encoder watchdog — every 150 game ticks (~5s at 30fps), check whether
	// the encoder has successfully written any frames since last check.
	// If not (silent UDP failure), close and reopen to re-resolve destination.
	// -----------------------------------------------------------------------
	const uint32 WatchdogInterval = static_cast<uint32>(FMath::Max(30, Config.EncoderWatchdogIntervalTicks));
	if (VideoEncoder && VideoEncoder->IsOpen() && FrameCntr > WatchdogInterval)
	{
		if ((FrameCntr - WatchdogLastCheckTick) >= WatchdogInterval)
		{
			const uint64 CurrentSuccess = VideoEncoder->GetSuccessfulFrameCount();
			if (CurrentSuccess == WatchdogLastSuccessFrame)
			{
				switch (Config.EncoderWatchdogPolicy)
				{
					case FCamSimConfig::EEncoderWatchdogPolicy::LogOnly:
						UE_LOG(LogCamSim, Warning,
							TEXT("UCamSimSubsystem: encoder watchdog — no frames written in %u ticks (log_only)"),
							WatchdogInterval);
						break;

					case FCamSimConfig::EEncoderWatchdogPolicy::FailFast:
						UE_LOG(LogCamSim, Fatal,
							TEXT("UCamSimSubsystem: encoder watchdog — no frames written in %u ticks (fail_fast)"),
							WatchdogInterval);
						break;

					case FCamSimConfig::EEncoderWatchdogPolicy::Reconnect:
					default:
						UE_LOG(LogCamSim, Warning,
							TEXT("UCamSimSubsystem: encoder watchdog — no frames written in %u ticks, reconnecting"),
							WatchdogInterval);
						++WatchdogReconnectCount;
						VideoEncoder->Close();
						if (!VideoEncoder->Open())
						{
							UE_LOG(LogCamSim, Error, TEXT("UCamSimSubsystem: encoder watchdog — reopen failed"));
						}
						break;
				}
			}
			WatchdogLastSuccessFrame = VideoEncoder->GetSuccessfulFrameCount();
			WatchdogLastCheckTick    = FrameCntr;
		}
	}

	// Runtime health snapshot every 150 ticks (~5s at 30fps)
	constexpr uint32 HealthInterval = 150;
	if (FrameCntr > 0 && (FrameCntr - HealthLastTick) >= HealthInterval)
	{
		const uint64 EncoderSuccess = (VideoEncoder && VideoEncoder->IsOpen())
			? VideoEncoder->GetSuccessfulFrameCount()
			: 0;
		const uint64 EncoderDelta = EncoderSuccess - HealthLastSuccessFrame;

		const uint64 RxPackets = CigiReceiver ? CigiReceiver->GetReceivedPacketCount() : 0;
		const uint64 RxDelta = RxPackets - HealthLastRxPacketCount;

		UE_LOG(LogCamSim, Log,
			TEXT("CamSimHealth: frame=%u encoder_open=%d enc_ok_total=%llu enc_ok_delta=%llu ")
			TEXT("cigi_rx_total=%llu cigi_rx_delta=%llu watchdog_reconnects=%u sender=%d query=%d"),
			FrameCntr,
			(VideoEncoder && VideoEncoder->IsOpen()) ? 1 : 0,
			EncoderSuccess, EncoderDelta,
			RxPackets, RxDelta,
			WatchdogReconnectCount,
			CigiSender ? 1 : 0,
			QueryHandler ? 1 : 0);

		HealthLastTick = FrameCntr;
		HealthLastSuccessFrame = EncoderSuccess;
		HealthLastRxPacketCount = RxPackets;
	}

	// Write Docker HEALTHCHECK file every 90 ticks (~3s at 30fps)
	if (FrameCntr % 90 == 0)
	{
		const FString HealthPath = FPaths::Combine(FPlatformProcess::BaseDir(), TEXT("camsim_health"));
		FFileHelper::SaveStringToFile(FString::FromInt(static_cast<int32>(FrameCntr)), *HealthPath);
	}

	++FrameCntr;
}
