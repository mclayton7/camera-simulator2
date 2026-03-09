// Copyright CamSim Contributors. All Rights Reserved.

#include "Subsystem/CamSimSubsystem.h"
#include "Entity/CamSimEntityManager.h"
#include "CIGI/CigiReceiver.h"
#include "CIGI/CigiSender.h"
#include "CIGI/CigiQueryHandler.h"
#include "Geospatial/CamSimGeospatialProvider.h"
#include "Encoder/MultiViewFrameSink.h" // FMultiViewFrameSink (concrete IFrameSink)
#include "Encoder/IFrameSink.h"
#include "Metadata/KlvBuilder.h"        // FKlvBuilder::SetSecurityMetadata (Phase 12A)
#include "CamSimTest.h"
#include "Engine/World.h"
#include "DynamicRHI.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

extern "C"
{
#include "libavcodec/version.h"
#include "libswscale/version.h"
}

// -------------------------------------------------------------------------
// Phase 13B: Pimpl — all owned sub-objects live here.
//
// TUniquePtr<FSubsystemImpl> in the header triggers correct cleanup on
// destruction/exception — no manual delete chains needed.
// -------------------------------------------------------------------------

struct UCamSimSubsystem::FSubsystemImpl
{
	// Owned components
	TUniquePtr<FCigiReceiver>        CigiReceiver;
	TUniquePtr<FMultiViewFrameSink>  VideoEncoder;
	TUniquePtr<FCamSimEntityManager> EntityManager;
	TUniquePtr<FCigiSender>          CigiSender;
	TUniquePtr<FCigiQueryHandler>    QueryHandler;
	TUniquePtr<FCamSimGeospatialProvider> GeospatialProvider;

	// IG frame counter — incremented each tick; sent in every SOF packet
	uint32 FrameCntr = 0;

	// Encoder watchdog
	uint64 WatchdogLastSuccessFrame = 0;
	uint32 WatchdogLastCheckTick    = 0;
	uint32 WatchdogReconnectCount   = 0;
	uint32 HealthFileTick           = 0;

	// Runtime health snapshot counters
	uint32 HealthLastTick           = 0;
	uint64 HealthLastSuccessFrame   = 0;
	uint64 HealthLastRxPacketCount  = 0;

	// Wall-clock start time
	double StartTimeSec             = 0.0;

	// IG mode: 0=Standby, 1=Operate
	uint8  IGMode                   = 0;

	// Prometheus metrics
	uint32 PrometheusLastTick       = 0;

	~FSubsystemImpl()
	{
		// Tear down in reverse-dependency order.
		// TUniquePtr destructors handle null checks automatically.
		QueryHandler.Reset();
		GeospatialProvider.Reset();
		if (CigiSender) CigiSender->Close();
		CigiSender.Reset();
		if (VideoEncoder) VideoEncoder->Close();
		VideoEncoder.Reset();
		EntityManager.Reset();
		if (CigiReceiver) CigiReceiver->Stop();
		CigiReceiver.Reset();
	}
};

// -------------------------------------------------------------------------
// Constructor / Destructor — defined here so the compiler sees the full
// FSubsystemImpl type when instantiating TUniquePtr's destructor.
// -------------------------------------------------------------------------

UCamSimSubsystem::UCamSimSubsystem() = default;
UCamSimSubsystem::~UCamSimSubsystem() = default;

// -------------------------------------------------------------------------
// Accessor implementations (Pimpl delegation)
// -------------------------------------------------------------------------

FCigiReceiver* UCamSimSubsystem::GetCigiReceiver() const
{
	return Impl ? Impl->CigiReceiver.Get() : nullptr;
}

IFrameSink* UCamSimSubsystem::GetVideoEncoder() const
{
	return Impl ? Impl->VideoEncoder.Get() : nullptr;
}

FCamSimEntityManager* UCamSimSubsystem::GetEntityManager() const
{
	return Impl ? Impl->EntityManager.Get() : nullptr;
}

FCigiSender* UCamSimSubsystem::GetCigiSender() const
{
	return Impl ? Impl->CigiSender.Get() : nullptr;
}

FCigiQueryHandler* UCamSimSubsystem::GetQueryHandler() const
{
	return Impl ? Impl->QueryHandler.Get() : nullptr;
}

FCamSimGeospatialProvider* UCamSimSubsystem::GetGeospatialProvider() const
{
	return Impl ? Impl->GeospatialProvider.Get() : nullptr;
}

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

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

// -------------------------------------------------------------------------
// Initialize
// -------------------------------------------------------------------------

void UCamSimSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Load config (YAML parse + env var overrides) — Phase 13C: check success flag
	Config = FCamSimConfig::Load();
	if (!Config.bLoadedSuccessfully)
	{
		UE_LOG(LogCamSim, Error, TEXT("UCamSimSubsystem: config load failed — using defaults"));
	}

	EntityTypeTable.LoadFromConfig();

	const FString RHIName = GDynamicRHI ? GDynamicRHI->GetName() : TEXT("Unknown");
	UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: initializing"));
	UE_LOG(LogCamSim, Log,
		TEXT("CamSim startup diagnostics: platform=%s rhi=%s ffmpeg(libavcodec=%d.%d.%d libswscale=%d.%d.%d) ")
		TEXT("video=udp://%s:%d cigi_in=%s:%d cigi_resp=%s:%d capture=%dx%d@%.1ffps ")
		TEXT("readback=%s swap_rb=%d ready_polls=%d bitrate=%d preset=%s tune=%s watchdog_policy=%s watchdog_ticks=%d ")
		TEXT("sensor_quality=%s views=%d ground_truth=%d"),
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
		WatchdogPolicyToString(Config.EncoderWatchdogPolicy), Config.EncoderWatchdogIntervalTicks,
		*Config.SensorQualityPreset, Config.OutputViews.Num(), Config.GroundTruth.bEnabled ? 1 : 0);

	// Initialise MISB ST 0102 security metadata for KLV output (Phase 12A)
	FKlvBuilder::SetSecurityMetadata(
		Config.SecurityMetadata.Classification,
		Config.SecurityMetadata.ClassifyingCountry,
		Config.SecurityMetadata.ObjectCountryCodes,
		Config.SecurityMetadata.Caveats,
		Config.SecurityMetadata.ReleasingInstructions);

	// Phase 13B: allocate Pimpl — all sub-objects owned via TUniquePtr
	Impl = MakeUnique<FSubsystemImpl>();

	// Start CIGI receiver thread
	Impl->CigiReceiver = MakeUnique<FCigiReceiver>(Config);
	if (!Impl->CigiReceiver->Start())
	{
		UE_LOG(LogCamSim, Error, TEXT("UCamSimSubsystem: failed to start CIGI receiver"));
	}

	// Start FFmpeg encoder / MPEG-TS muxer(s)
	Impl->VideoEncoder = MakeUnique<FMultiViewFrameSink>(Config);
	if (!Impl->VideoEncoder->Open())
	{
		UE_LOG(LogCamSim, Error, TEXT("UCamSimSubsystem: failed to open video encoder"));
	}

	// Create entity manager (game thread tickable — no explicit start needed)
	Impl->EntityManager = MakeUnique<FCamSimEntityManager>(this, &EntityTypeTable);
	UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: entity manager created"));

	Impl->GeospatialProvider = MakeUnique<FCamSimGeospatialProvider>(Config);
	if (Impl->GeospatialProvider)
	{
		const FCamSimGeospatialCapabilities& Caps = Impl->GeospatialProvider->GetCapabilities();
		UE_LOG(LogCamSim, Log,
			TEXT("UCamSimSubsystem: geospatial provider=%s caps(transforms=%d terrain_queries=%d)"),
			*Impl->GeospatialProvider->GetProviderName(),
			Caps.bSupportsGeoreferenceTransforms ? 1 : 0,
			Caps.bSupportsTerrainLineTraceQueries ? 1 : 0);
	}

	// Start CIGI sender (IG → host: SOF heartbeat + HAT/HOT + LOS responses)
	Impl->CigiSender = MakeUnique<FCigiSender>();
	if (!Impl->CigiSender->Open(Config))
	{
		UE_LOG(LogCamSim, Warning, TEXT("UCamSimSubsystem: CIGI sender failed to open (responses disabled)"));
	}

	// Create query handler (drains HAT/HOT + LOS queues, runs line traces)
	Impl->QueryHandler = MakeUnique<FCigiQueryHandler>(this, Impl->CigiSender.Get());
	UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: CIGI query handler created"));

	// Register for graceful shutdown on SIGTERM (Phase 2)
	// Phase 13C: Use weak lambda to guard against dangling this pointer
	FCoreDelegates::GetApplicationWillTerminateDelegate().AddLambda([this]()
	{
		UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: ApplicationWillTerminate — flushing encoder"));
		if (Impl)
		{
			if (Impl->VideoEncoder) Impl->VideoEncoder->Close();
			if (Impl->CigiSender)   Impl->CigiSender->Close();
			if (Impl->CigiReceiver) Impl->CigiReceiver->Stop();
		}
	});

	Impl->StartTimeSec = FPlatformTime::Seconds();
}

// -------------------------------------------------------------------------
// Deinitialize
// -------------------------------------------------------------------------

void UCamSimSubsystem::Deinitialize()
{
	UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: shutting down"));

	// Phase 13B: Pimpl destructor handles reverse-order teardown
	Impl.Reset();

	Super::Deinitialize();
}

// -------------------------------------------------------------------------
// Tick
// -------------------------------------------------------------------------

void UCamSimSubsystem::Tick(float DeltaTime)
{
	if (!Impl) return;

	// Drain HAT/HOT + LOS query queues and stage responses
	if (Impl->QueryHandler)
	{
		Impl->QueryHandler->Tick(DeltaTime);
	}

	// Determine IG operating mode (Phase 12D):
	// 0=Standby (no CIGI packets received yet), 1=Operate
	if (Impl->IGMode == 0 && Impl->CigiReceiver && Impl->CigiReceiver->GetReceivedPacketCount() > 0)
	{
		Impl->IGMode = 1;
		UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: IG mode -> Operate (first CIGI packet received)"));
	}

	// Flush SOF + all staged responses into one UDP datagram
	if (Impl->CigiSender)
	{
		const uint32 LastHostFrame = Impl->CigiReceiver ? Impl->CigiReceiver->GetLastHostFrame() : 0;
		Impl->CigiSender->FlushFrame(Impl->FrameCntr, static_cast<uint8>(LastHostFrame), Impl->IGMode);
	}

	// -----------------------------------------------------------------------
	// Encoder watchdog — every N game ticks, check for silent stream death
	// -----------------------------------------------------------------------
	const uint32 WatchdogInterval = static_cast<uint32>(FMath::Max(30, Config.EncoderWatchdogIntervalTicks));
	IFrameSink* Encoder = Impl->VideoEncoder.Get();
	if (Encoder && Encoder->IsOpen() && Impl->FrameCntr > WatchdogInterval)
	{
		if ((Impl->FrameCntr - Impl->WatchdogLastCheckTick) >= WatchdogInterval)
		{
			const uint64 CurrentSuccess = Encoder->GetSuccessfulFrameCount();
			if (CurrentSuccess == Impl->WatchdogLastSuccessFrame)
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
						++Impl->WatchdogReconnectCount;
						if (Config.WatchdogMaxReconnects > 0 &&
							static_cast<int32>(Impl->WatchdogReconnectCount) >= Config.WatchdogMaxReconnects)
						{
							UE_LOG(LogCamSim, Error,
								TEXT("UCamSimSubsystem: encoder watchdog — %u reconnects exhausted, failing fast"),
								Impl->WatchdogReconnectCount);
							FPlatformMisc::RequestExit(true);
							break;
						}
						UE_LOG(LogCamSim, Warning,
							TEXT("UCamSimSubsystem: encoder watchdog — no frames written in %u ticks, reconnecting (%u/%d)"),
							WatchdogInterval, Impl->WatchdogReconnectCount, Config.WatchdogMaxReconnects);
						Encoder->Close();
						if (!Encoder->Open())
						{
							UE_LOG(LogCamSim, Error, TEXT("UCamSimSubsystem: encoder watchdog — reopen failed"));
						}
						break;
				}
			}
			Impl->WatchdogLastSuccessFrame = Encoder->GetSuccessfulFrameCount();
			Impl->WatchdogLastCheckTick    = Impl->FrameCntr;
		}
	}

	// Runtime health snapshot every 150 ticks (~5s at 30fps)
	constexpr uint32 HealthInterval = 150;
	if (Impl->FrameCntr > 0 && (Impl->FrameCntr - Impl->HealthLastTick) >= HealthInterval)
	{
		const uint64 EncoderSuccess = (Encoder && Encoder->IsOpen())
			? Encoder->GetSuccessfulFrameCount()
			: 0;
		const uint64 EncoderDelta = EncoderSuccess - Impl->HealthLastSuccessFrame;

		const uint64 RxPackets = Impl->CigiReceiver ? Impl->CigiReceiver->GetReceivedPacketCount() : 0;
		const uint64 RxDelta = RxPackets - Impl->HealthLastRxPacketCount;

		const uint32 HostFrame = Impl->CigiReceiver ? Impl->CigiReceiver->GetLastHostFrame() : 0;

		UE_LOG(LogCamSim, Log,
			TEXT("CamSimHealth: frame=%u encoder_open=%d enc_ok_total=%llu enc_ok_delta=%llu ")
			TEXT("cigi_rx_total=%llu cigi_rx_delta=%llu watchdog_reconnects=%u sender=%d query=%d host_frame=%u"),
			Impl->FrameCntr,
			(Encoder && Encoder->IsOpen()) ? 1 : 0,
			EncoderSuccess, EncoderDelta,
			RxPackets, RxDelta,
			Impl->WatchdogReconnectCount,
			Impl->CigiSender ? 1 : 0,
			Impl->QueryHandler ? 1 : 0,
			HostFrame);

		Impl->HealthLastTick = Impl->FrameCntr;
		Impl->HealthLastSuccessFrame = EncoderSuccess;
		Impl->HealthLastRxPacketCount = RxPackets;
	}

	// Write structured health JSON every 90 ticks (~3s at 30fps) for Docker HEALTHCHECK
	if (Impl->FrameCntr > 0 && (Impl->FrameCntr - Impl->HealthFileTick) >= 90)
	{
		const uint64 EncOk = (Encoder && Encoder->IsOpen())
			? Encoder->GetSuccessfulFrameCount() : 0;
		const uint64 CigiRx = Impl->CigiReceiver ? Impl->CigiReceiver->GetReceivedPacketCount() : 0;
		const uint32 LastHost = Impl->CigiReceiver ? Impl->CigiReceiver->GetLastHostFrame() : 0;
		const double UptimeSec = FPlatformTime::Seconds() - Impl->StartTimeSec;

		const FString HealthJson = FString::Printf(
			TEXT("{\"frame\":%u,\"encoder_ok\":%s,\"cigi_rx\":%llu,\"dropped\":%u,\"uptime_s\":%.1f,\"last_host_frame\":%u}"),
			Impl->FrameCntr,
			(Encoder && Encoder->IsOpen()) ? TEXT("true") : TEXT("false"),
			EncOk,
			Impl->WatchdogReconnectCount,
			UptimeSec,
			LastHost);

		const FString HealthPath = FPaths::Combine(FPlatformProcess::BaseDir(), TEXT("camsim_health.json"));
		if (!FFileHelper::SaveStringToFile(HealthJson, *HealthPath))
		{
			UE_LOG(LogCamSim, Warning, TEXT("UCamSimSubsystem: failed to write health file %s"), *HealthPath);
		}
		Impl->HealthFileTick = Impl->FrameCntr;
	}

	// Write Prometheus-compatible metrics file (Phase 12D)
	if (!Config.PrometheusMetricsPath.IsEmpty() && Impl->FrameCntr > 0
		&& (Impl->FrameCntr - Impl->PrometheusLastTick) >= 90)
	{
		const uint64 EncOk = (Encoder && Encoder->IsOpen())
			? Encoder->GetSuccessfulFrameCount() : 0;
		const uint64 CigiRx = Impl->CigiReceiver ? Impl->CigiReceiver->GetReceivedPacketCount() : 0;
		const double UptimeSec = FPlatformTime::Seconds() - Impl->StartTimeSec;

		const FString Prom = FString::Printf(
			TEXT("# HELP camsim_frame_count Total game ticks\n"
			     "# TYPE camsim_frame_count counter\n"
			     "camsim_frame_count %u\n"
			     "# HELP camsim_encoder_frames_total Total successfully encoded frames\n"
			     "# TYPE camsim_encoder_frames_total counter\n"
			     "camsim_encoder_frames_total %llu\n"
			     "# HELP camsim_encoder_ok Whether encoder is open\n"
			     "# TYPE camsim_encoder_ok gauge\n"
			     "camsim_encoder_ok %d\n"
			     "# HELP camsim_cigi_rx_total Total CIGI packets received\n"
			     "# TYPE camsim_cigi_rx_total counter\n"
			     "camsim_cigi_rx_total %llu\n"
			     "# HELP camsim_uptime_seconds Uptime in seconds\n"
			     "# TYPE camsim_uptime_seconds gauge\n"
			     "camsim_uptime_seconds %.1f\n"
			     "# HELP camsim_watchdog_reconnects_total Total encoder watchdog reconnects\n"
			     "# TYPE camsim_watchdog_reconnects_total counter\n"
			     "camsim_watchdog_reconnects_total %u\n"
			     "# HELP camsim_ig_mode IG operating mode (0=Standby 1=Operate)\n"
			     "# TYPE camsim_ig_mode gauge\n"
			     "camsim_ig_mode %u\n"),
			Impl->FrameCntr,
			EncOk,
			(Encoder && Encoder->IsOpen()) ? 1 : 0,
			CigiRx,
			UptimeSec,
			Impl->WatchdogReconnectCount,
			static_cast<uint32>(Impl->IGMode));

		if (!FFileHelper::SaveStringToFile(Prom, *Config.PrometheusMetricsPath))
		{
			UE_LOG(LogCamSim, Warning, TEXT("UCamSimSubsystem: failed to write prometheus metrics"));
		}
		Impl->PrometheusLastTick = Impl->FrameCntr;
	}

	++Impl->FrameCntr;
}
