// Copyright CamSim Contributors. All Rights Reserved.

#include "Subsystem/CamSimSubsystem.h"
#include "Camera/CamSimCamera.h"
#include "Camera/CamSimSensorComponent.h"
#include "Entity/CamSimEntityManager.h"
#include "CIGI/CigiReceiver.h"
#include "CIGI/CigiSender.h"
#include "CIGI/CigiQueryHandler.h"
#include "Geospatial/CamSimGeospatialProvider.h"
#include "Encoder/MultiViewFrameSink.h" // FMultiViewFrameSink (concrete IFrameSink)
#include "Encoder/IFrameSink.h"
#include "Metadata/KlvBuilder.h"        // FKlvBuilder::SetSecurityMetadata (Phase 12A)
#include "GroundTruth/FGroundTruthCollector.h"
#include "Environment/CamSimParticleManager.h"
#include "DIS/DisReceiver.h"
#include "DIS/DisEntityAdapter.h"
#include "Streaming/CotSender.h"
#include "Logging/CamSimJsonLogger.h"
#include "Diagnostics/PipelineLatencyTracker.h"
#include "Health/CamSimHealthServer.h"
#include "CamSimTest.h"
#include "Engine/World.h"
#include "DynamicRHI.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "CesiumIonServer.h"
#include "UObject/StrongObjectPtr.h"

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
	TUniquePtr<FGroundTruthCollector>     GroundTruthCollector;
	TUniquePtr<FCamSimParticleManager>    ParticleManager;
	TUniquePtr<FDisReceiver>             DisReceiver;
	TUniquePtr<FDisEntityAdapter>        DisAdapter;
	TUniquePtr<FCotSender>               CotSender;
	TUniquePtr<FCamSimJsonLogger>        JsonLogger;
	TUniquePtr<FPipelineLatencyTracker>  LatencyTracker;
	TUniquePtr<FCamSimHealthServer>     HealthServer;

	// Transient UCesiumIonServer created when CesiumBackend config overrides defaults.
	// TStrongObjectPtr prevents GC of this UDataAsset-derived object from a plain C++ struct.
	TStrongObjectPtr<UCesiumIonServer> CesiumIonServerOverride;

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
	double HealthLastWallSec        = 0.0;

	// Wall-clock start time
	double StartTimeSec             = 0.0;

	// IG mode: 0=Standby, 1=Operate
	uint8  IGMode                   = 0;

	// Prometheus metrics
	uint32 PrometheusLastTick       = 0;

	// §10.4 metrics: rolling-1-second FPS measurement (updated from Tick).
	uint32 FpsLastSampleFrameCntr     = 0;
	uint64 FpsLastSampleEncodedFrames = 0;
	double FpsLastSampleWallSec       = 0.0;
	double CachedRenderFps            = 0.0;  // measured render ticks/sec
	double CachedOutputFps            = 0.0;  // measured encoded frames/sec

	~FSubsystemImpl()
	{
		// Tear down in reverse-dependency order.
		// TUniquePtr destructors handle null checks automatically.
		if (HealthServer) { HealthServer->Stop(); }
		HealthServer.Reset();
		CesiumIonServerOverride.Reset();
		QueryHandler.Reset();
		GeospatialProvider.Reset();
		if (GroundTruthCollector) { GroundTruthCollector->Close(); }
		GroundTruthCollector.Reset();
		ParticleManager.Reset();
		if (CigiSender) CigiSender->Close();
		CigiSender.Reset();
		if (VideoEncoder) VideoEncoder->Close();
		VideoEncoder.Reset();
		EntityManager.Reset();
		if (CotSender) CotSender->Close();
		CotSender.Reset();
		DisAdapter.Reset();
		if (DisReceiver) DisReceiver->Stop();
		DisReceiver.Reset();
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

FGroundTruthCollector* UCamSimSubsystem::GetGroundTruthCollector() const
{
	return Impl ? Impl->GroundTruthCollector.Get() : nullptr;
}

FCamSimParticleManager* UCamSimSubsystem::GetParticleManager() const
{
	return Impl ? Impl->ParticleManager.Get() : nullptr;
}

FDisReceiver* UCamSimSubsystem::GetDisReceiver() const
{
	return Impl ? Impl->DisReceiver.Get() : nullptr;
}

FDisEntityAdapter* UCamSimSubsystem::GetDisAdapter() const
{
	return Impl ? Impl->DisAdapter.Get() : nullptr;
}

FCotSender* UCamSimSubsystem::GetCotSender() const
{
	return Impl ? Impl->CotSender.Get() : nullptr;
}

FPipelineLatencyTracker* UCamSimSubsystem::GetLatencyTracker() const
{
	return Impl ? Impl->LatencyTracker.Get() : nullptr;
}

void UCamSimSubsystem::RegisterCamera(ACamSimCamera* Camera)
{
	Camera_ = Camera;
}

ACamSimCamera* UCamSimSubsystem::GetCamera() const
{
	return Camera_.Get();
}

void UCamSimSubsystem::HotReloadConfig(const FCamSimConfig& NewCfg)
{
	// Preserve immutable fields that cannot change without a restart.
	const int32   SavedCigiPort      = Config.CigiPort;
	const FString SavedMulticastAddr = Config.MulticastAddr;
	const int32   SavedMulticastPort = Config.MulticastPort;
	const FString SavedVideoCodec    = Config.VideoCodec;

	Config = NewCfg;

	Config.CigiPort      = SavedCigiPort;
	Config.MulticastAddr = SavedMulticastAddr;
	Config.MulticastPort = SavedMulticastPort;
	Config.VideoCodec    = SavedVideoCodec;

	// Phase 22A: Re-parse entity types on hot-reload
	EntityTypeTable.HotReload();
}

void UCamSimSubsystem::StoreCesiumIonServer(UCesiumIonServer* Server)
{
	if (Impl)
	{
		Impl->CesiumIonServerOverride.Reset();
		if (Server)
		{
			Impl->CesiumIonServerOverride = TStrongObjectPtr<UCesiumIonServer>(Server);
		}
	}
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

	// Phase 28D: pre-flight config validation
	{
		const TArray<FString> ValidationErrors = Config.Validate();
		for (const FString& Err : ValidationErrors)
		{
			UE_LOG(LogCamSim, Error, TEXT("Config validation: %s"), *Err);
		}
		if (ValidationErrors.Num() > 0)
		{
			UE_LOG(LogCamSim, Error, TEXT("UCamSimSubsystem: %d config validation error(s) — check config"),
				ValidationErrors.Num());
			Config.bLoadedSuccessfully = false;
		}
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
		*Config.SensorQualityPreset, Config.OutputViews.Num(), Config.MLTraining.bEnabled ? 1 : 0);

	// Initialise MISB ST 0102 security metadata for KLV output (Phase 12A)
	FKlvBuilder::SetSecurityMetadata(
		Config.SecurityMetadata.Classification,
		Config.SecurityMetadata.ClassifyingCountry,
		Config.SecurityMetadata.ObjectCountryCodes,
		Config.SecurityMetadata.Caveats,
		Config.SecurityMetadata.ReleasingInstructions);

	// Phase 26: configure KLV checksum algorithm, tail number, and target gate
	FKlvBuilder::Configure(
		Config.Phase26.KlvChecksum,
		Config.Phase26.PlatformTailNumber,
		Config.Phase26.TargetTrackGateWidth,
		Config.Phase26.TargetTrackGateHeight);

	// Phase 13B: allocate Pimpl — all sub-objects owned via TUniquePtr
	Impl = MakePimpl<FSubsystemImpl>();

	// Phase 28B: structured JSON logging
	if (!Config.Operational.StructuredLogPath.IsEmpty())
	{
		Impl->JsonLogger = MakeUnique<FCamSimJsonLogger>();
		if (Impl->JsonLogger->Open(Config.Operational.StructuredLogPath,
		                            Config.Operational.StructuredLogMaxMB))
		{
			// Log startup config digest
			TMap<FString, FString> StartupFields;
			StartupFields.Add(TEXT("capture"), FString::Printf(TEXT("%dx%d"), Config.CaptureWidth, Config.CaptureHeight));
			StartupFields.Add(TEXT("codec"), Config.VideoCodec);
			StartupFields.Add(TEXT("bitrate"), FString::FromInt(Config.VideoBitrate));
			StartupFields.Add(TEXT("fps"), FString::SanitizeFloat(Config.FrameRate));
			Impl->JsonLogger->Log(TEXT("info"), TEXT("subsystem"), TEXT("initialized"), StartupFields);
		}
		else
		{
			UE_LOG(LogCamSim, Warning, TEXT("UCamSimSubsystem: failed to open structured log at %s"),
				*Config.Operational.StructuredLogPath);
			Impl->JsonLogger.Reset();
		}
	}

	// Phase 28G: pipeline latency tracker
	if (Config.Performance.bTrackPipelineLatency)
	{
		const int32 BufSize = static_cast<int32>(Config.Performance.OutputFrameRateHz * 10.0f);
		Impl->LatencyTracker = MakeUnique<FPipelineLatencyTracker>(BufSize);
		UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: pipeline latency tracking enabled (buffer=%d)"), BufSize);
	}

	// Phase 28C: HTTP health endpoints
	if (Config.Operational.bHealthHttpEnabled)
	{
		Impl->HealthServer = MakeUnique<FCamSimHealthServer>();
		auto* ImplPtr = Impl.Get();
		Impl->HealthServer->Start(Config.Operational.HealthHttpPort,
			// IsAlive — always true if we got this far
			[ImplPtr]() { return true; },
			// IsEncoderReady
			[ImplPtr]() { return ImplPtr->VideoEncoder && ImplPtr->VideoEncoder->IsOpen(); },
			// IsCigiReady
			[ImplPtr]() { return ImplPtr->CigiReceiver && ImplPtr->CigiReceiver->GetReceivedPacketCount() > 0; },
			// HasFirstFrame
			[ImplPtr]() { return ImplPtr->VideoEncoder && ImplPtr->VideoEncoder->GetSuccessfulFrameCount() > 0; },
			// §10.4 contract: gauges render_fps, output_fps, entity_count, uptime_seconds;
			// counters frame_drops_total, cigi_packets_total, dis_packets_total, frames_encoded_total.
			// Optional histogram frame_latency_ms when pipeline latency tracking is enabled.
			[ImplPtr, this]() -> FString
			{
				const uint64 EncodedFrames = (ImplPtr->VideoEncoder && ImplPtr->VideoEncoder->IsOpen())
					? ImplPtr->VideoEncoder->GetSuccessfulFrameCount() : 0;
				const uint64 CigiRx = ImplPtr->CigiReceiver
					? ImplPtr->CigiReceiver->GetReceivedPacketCount() : 0;
				const uint64 DisRx = ImplPtr->DisReceiver
					? ImplPtr->DisReceiver->GetReceivedPacketCount() : 0;
				const double Uptime = FPlatformTime::Seconds() - ImplPtr->StartTimeSec;
				const int32 EntityCount = ImplPtr->EntityManager
					? ImplPtr->EntityManager->GetEntityCount() : 0;

				int32 FrameDropsTotal = 0;
				if (ACamSimCamera* Cam = Camera_.Get())
				{
					if (Cam->IsTrackingFrameDrops())
					{
						FrameDropsTotal = Cam->GetFrameDropStats().Total();
					}
				}

				FString Body;
				Body.Reserve(2048);

				Body += TEXT("# HELP camsim_render_fps Current render thread framerate.\n");
				Body += TEXT("# TYPE camsim_render_fps gauge\n");
				Body += FString::Printf(TEXT("camsim_render_fps %.3f\n"), ImplPtr->CachedRenderFps);

				Body += TEXT("# HELP camsim_output_fps Current encoder output framerate.\n");
				Body += TEXT("# TYPE camsim_output_fps gauge\n");
				Body += FString::Printf(TEXT("camsim_output_fps %.3f\n"), ImplPtr->CachedOutputFps);

				Body += TEXT("# HELP camsim_entity_count Active entities in the scene.\n");
				Body += TEXT("# TYPE camsim_entity_count gauge\n");
				Body += FString::Printf(TEXT("camsim_entity_count %d\n"), EntityCount);

				Body += TEXT("# HELP camsim_uptime_seconds Subsystem uptime in seconds.\n");
				Body += TEXT("# TYPE camsim_uptime_seconds gauge\n");
				Body += FString::Printf(TEXT("camsim_uptime_seconds %.3f\n"), Uptime);

				Body += TEXT("# HELP camsim_frame_drops_total Total frame drops across all categories.\n");
				Body += TEXT("# TYPE camsim_frame_drops_total counter\n");
				Body += FString::Printf(TEXT("camsim_frame_drops_total %d\n"), FrameDropsTotal);

				Body += TEXT("# HELP camsim_cigi_packets_total Total CIGI packets received.\n");
				Body += TEXT("# TYPE camsim_cigi_packets_total counter\n");
				Body += FString::Printf(TEXT("camsim_cigi_packets_total %llu\n"),
					static_cast<unsigned long long>(CigiRx));

				Body += TEXT("# HELP camsim_dis_packets_total Total DIS PDUs received.\n");
				Body += TEXT("# TYPE camsim_dis_packets_total counter\n");
				Body += FString::Printf(TEXT("camsim_dis_packets_total %llu\n"),
					static_cast<unsigned long long>(DisRx));

				Body += TEXT("# HELP camsim_frames_encoded_total Total frames successfully encoded.\n");
				Body += TEXT("# TYPE camsim_frames_encoded_total counter\n");
				Body += FString::Printf(TEXT("camsim_frames_encoded_total %llu\n"),
					static_cast<unsigned long long>(EncodedFrames));

				// Optional histogram: only when latency tracking is enabled.
				if (ImplPtr->LatencyTracker)
				{
					const FPipelineLatencyTracker::FLatencyPercentiles P =
						ImplPtr->LatencyTracker->ComputePercentiles();
					Body += TEXT("# HELP camsim_frame_latency_ms Per-frame pipeline latency.\n");
					Body += TEXT("# TYPE camsim_frame_latency_ms summary\n");
					Body += FString::Printf(TEXT("camsim_frame_latency_ms{quantile=\"0.5\"} %.3f\n"),
						P.TotalUs[0] / 1000.0);
					Body += FString::Printf(TEXT("camsim_frame_latency_ms{quantile=\"0.95\"} %.3f\n"),
						P.TotalUs[1] / 1000.0);
					Body += FString::Printf(TEXT("camsim_frame_latency_ms{quantile=\"0.99\"} %.3f\n"),
						P.TotalUs[2] / 1000.0);
				}

				return Body;
			}
		);
	}

	// Start CIGI receiver thread
	Impl->CigiReceiver = MakeUnique<FCigiReceiver>(Config);
	if (!Impl->CigiReceiver->Start())
	{
		UE_LOG(LogCamSim, Error, TEXT("UCamSimSubsystem: failed to start CIGI receiver"));
	}

	// Start DIS receiver thread (Phase 21) — runs alongside CIGI
	if (Config.DIS.bEnabled)
	{
		Impl->DisReceiver = MakeUnique<FDisReceiver>(Config);
		if (!Impl->DisReceiver->Start())
		{
			UE_LOG(LogCamSim, Error, TEXT("UCamSimSubsystem: failed to start DIS receiver"));
		}
		Impl->DisAdapter = MakeUnique<FDisEntityAdapter>(Config, Impl->DisReceiver.Get());
		UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: DIS protocol enabled (port=%d exercise=%d)"),
			Config.DIS.Port, Config.DIS.ExerciseId);
	}

	// Start CoT sender (Phase 21D.1)
	if (Config.Streaming.bCotEnabled)
	{
		Impl->CotSender = MakeUnique<FCotSender>(Config);
		if (!Impl->CotSender->Open())
		{
			UE_LOG(LogCamSim, Warning, TEXT("UCamSimSubsystem: CoT sender failed to open"));
		}
		UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: CoT enabled (addr=%s:%d interval=%.1fs)"),
			*Config.Streaming.CotAddr, Config.Streaming.CotPort, Config.Streaming.CotIntervalSec);
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

	// Create ground truth / ML training data collector (Phase 17)
	Impl->GroundTruthCollector = MakeUnique<FGroundTruthCollector>(Config);
	if (!Impl->GroundTruthCollector->Open())
	{
		UE_LOG(LogCamSim, Warning, TEXT("UCamSimSubsystem: ground truth collector failed to open"));
	}

	// Create particle effect manager (Phase 18F/G/H/I)
	Impl->ParticleManager = MakeUnique<FCamSimParticleManager>(this);
	Impl->ParticleManager->Initialize(Config);
	UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: particle manager created"));

	// Register for graceful shutdown on SIGTERM (Phase 2)
	// Phase 13C: Use weak lambda to guard against dangling this pointer
	FCoreDelegates::GetApplicationWillTerminateDelegate().AddLambda([this]()
	{
		UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: ApplicationWillTerminate — flushing encoder"));
		if (Impl)
		{
			if (Impl->HealthServer) { Impl->HealthServer->Stop(); }
			if (Impl->JsonLogger)
			{
				TMap<FString, FString> ShutdownFields;
				ShutdownFields.Add(TEXT("frame"), FString::FromInt(Impl->FrameCntr));
				ShutdownFields.Add(TEXT("uptime_s"), FString::SanitizeFloat(
					FPlatformTime::Seconds() - Impl->StartTimeSec));
				Impl->JsonLogger->Log(TEXT("info"), TEXT("subsystem"), TEXT("shutdown"), ShutdownFields);
				Impl->JsonLogger->Flush();
			}
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

	// §10.4 metrics: update render/output FPS once per second.
	{
		const double NowSec = FPlatformTime::Seconds();
		if (Impl->FpsLastSampleWallSec == 0.0)
		{
			Impl->FpsLastSampleWallSec       = NowSec;
			Impl->FpsLastSampleFrameCntr     = Impl->FrameCntr;
			Impl->FpsLastSampleEncodedFrames = (Impl->VideoEncoder && Impl->VideoEncoder->IsOpen())
				? Impl->VideoEncoder->GetSuccessfulFrameCount() : 0;
		}
		else
		{
			const double Elapsed = NowSec - Impl->FpsLastSampleWallSec;
			if (Elapsed >= 1.0)
			{
				const uint32 FrameDelta = Impl->FrameCntr - Impl->FpsLastSampleFrameCntr;
				const uint64 CurrentEncoded = (Impl->VideoEncoder && Impl->VideoEncoder->IsOpen())
					? Impl->VideoEncoder->GetSuccessfulFrameCount() : 0;
				const uint64 EncodedDelta = CurrentEncoded - Impl->FpsLastSampleEncodedFrames;

				Impl->CachedRenderFps = FrameDelta / Elapsed;
				Impl->CachedOutputFps = EncodedDelta / Elapsed;

				Impl->FpsLastSampleWallSec       = NowSec;
				Impl->FpsLastSampleFrameCntr     = Impl->FrameCntr;
				Impl->FpsLastSampleEncodedFrames = CurrentEncoded;
			}
		}
	}

	// Phase 28C: update HTTP health liveness timestamp
	if (Impl->HealthServer)
	{
		Impl->HealthServer->UpdateTick();
	}

	// Drain HAT/HOT + LOS query queues and stage responses
	if (Impl->QueryHandler)
	{
		Impl->QueryHandler->Tick(DeltaTime);
	}

	// Tick particle effect manager (Phase 18F/G/H/I)
	if (Impl->ParticleManager)
	{
		Impl->ParticleManager->Tick(DeltaTime);
	}

	// Tick CoT sender (Phase 21D.1)
	if (Impl->CotSender)
	{
		// Get telemetry from camera if available
		FCamSimTelemetry CotTelemetry;
		if (ACamSimCamera* Cam = Camera_.Get())
		{
			CotTelemetry = Cam->GetCurrentTelemetry();
		}
		Impl->CotSender->Tick(DeltaTime, CotTelemetry);
	}

	// Determine IG operating mode (Phase 12D):
	// 0=Standby (no CIGI packets received yet), 1=Operate
	if (Impl->IGMode == 0 && Impl->CigiReceiver && Impl->CigiReceiver->GetReceivedPacketCount() > 0)
	{
		Impl->IGMode = 1;
		UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: IG mode -> Operate (first CIGI packet received)"));
	}

	// Stage Sensor Extended Response from camera telemetry
	if (Impl->CigiSender)
	{
		if (ACamSimCamera* Cam = Camera_.Get())
		{
			FCamSimTelemetry Telem = Cam->GetCurrentTelemetry();
			// SensorStat: 1=Tracking when sensor is on, 0=Searching when off.
			// SensorComp is private on ACamSimCamera; go through the public accessor.
			UCamSimSensorComponent* SensorComp = Cam->GetSensorComp();
			const uint8 SensorStat = (SensorComp && SensorComp->IsOn()) ? 1 : 0;
			// GateXoff/GateYoff: 0.0 = centered (no pixel-level tracking)
			Impl->CigiSender->SetSensorResponse(
				0,                           // ViewId
				Telem.SensorMode,            // SensorId (0=EO, 1=IR, 2=NVG)
				SensorStat,
				0.0f, 0.0f,                  // GateXoff, GateYoff (centered)
				0, 0,                        // GateSzX, GateSzY (no gate dimensions)
				Telem.FrameCenterLat,        // TrackPntLat (degrees)
				Telem.FrameCenterLon,        // TrackPntLon (degrees)
				Telem.FrameCenterElev,       // TrackPntAlt (metres)
				Impl->FrameCntr
			);
		}
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

		const double NowSec = FPlatformTime::Seconds();
		const double UptimeSec = NowSec - Impl->StartTimeSec;
		const double WallDeltaSec = (Impl->HealthLastWallSec > 0.0)
			? (NowSec - Impl->HealthLastWallSec)
			: 0.0;
		const uint32 TickDelta = Impl->FrameCntr - Impl->HealthLastTick;
		const double EffectiveFps = (WallDeltaSec > 0.0) ? (TickDelta / WallDeltaSec) : 0.0;

		UE_LOG(LogCamSim, Log,
			TEXT("CamSimHealth: frame=%u encoder_open=%d enc_ok_total=%llu enc_ok_delta=%llu ")
			TEXT("cigi_rx_total=%llu cigi_rx_delta=%llu watchdog_reconnects=%u sender=%d query=%d host_frame=%u ")
			TEXT("wall_dt=%.1fs fps=%.1f uptime=%.0fs"),
			Impl->FrameCntr,
			(Encoder && Encoder->IsOpen()) ? 1 : 0,
			EncoderSuccess, EncoderDelta,
			RxPackets, RxDelta,
			Impl->WatchdogReconnectCount,
			Impl->CigiSender ? 1 : 0,
			Impl->QueryHandler ? 1 : 0,
			HostFrame,
			WallDeltaSec, EffectiveFps, UptimeSec);

		Impl->HealthLastTick = Impl->FrameCntr;
		Impl->HealthLastSuccessFrame = EncoderSuccess;
		Impl->HealthLastRxPacketCount = RxPackets;
		Impl->HealthLastWallSec = NowSec;
	}

	// Write structured health JSON every 90 ticks (~3s at 30fps) for Docker HEALTHCHECK
	if (Impl->FrameCntr > 0 && (Impl->FrameCntr - Impl->HealthFileTick) >= 90)
	{
		const uint64 EncOk = (Encoder && Encoder->IsOpen())
			? Encoder->GetSuccessfulFrameCount() : 0;
		const uint64 CigiRx = Impl->CigiReceiver ? Impl->CigiReceiver->GetReceivedPacketCount() : 0;
		const uint32 LastHost = Impl->CigiReceiver ? Impl->CigiReceiver->GetLastHostFrame() : 0;
		const double UptimeSec = FPlatformTime::Seconds() - Impl->StartTimeSec;

		FString HealthJson = FString::Printf(
			TEXT("{\"frame\":%u,\"encoder_ok\":%s,\"cigi_rx\":%llu,\"dropped\":%u,\"uptime_s\":%.1f,\"last_host_frame\":%u"),
			Impl->FrameCntr,
			(Encoder && Encoder->IsOpen()) ? TEXT("true") : TEXT("false"),
			EncOk,
			Impl->WatchdogReconnectCount,
			UptimeSec,
			LastHost);

		// Phase 27B — append per-category frame drop stats when tracking is enabled
		if (ACamSimCamera* Cam = Camera_.Get())
		{
			if (Cam->IsTrackingFrameDrops())
			{
				const FFrameDropStats& D = Cam->GetFrameDropStats();
				HealthJson += FString::Printf(
					TEXT(",\"frame_drops\":{\"encoder_busy\":%d,\"readback_timeout\":%d,\"socket_error\":%d,\"total\":%d}"),
					D.EncoderBusy.Load(), D.ReadbackTimeout.Load(), D.SocketError.Load(), D.Total());
			}
		}
		// Phase 28G: pipeline latency percentiles
		if (Impl->LatencyTracker)
		{
			auto P = Impl->LatencyTracker->ComputePercentiles();
			HealthJson += FString::Printf(
				TEXT(",\"latency_us\":{\"readback_p50\":%.0f,\"readback_p95\":%.0f,\"readback_p99\":%.0f,")
				TEXT("\"sensor_p50\":%.0f,\"sensor_p95\":%.0f,\"sensor_p99\":%.0f,")
				TEXT("\"encode_p50\":%.0f,\"encode_p95\":%.0f,\"encode_p99\":%.0f,")
				TEXT("\"total_p50\":%.0f,\"total_p95\":%.0f,\"total_p99\":%.0f}"),
				P.ReadbackUs[0], P.ReadbackUs[1], P.ReadbackUs[2],
				P.SensorUs[0], P.SensorUs[1], P.SensorUs[2],
				P.EncodeUs[0], P.EncodeUs[1], P.EncodeUs[2],
				P.TotalUs[0], P.TotalUs[1], P.TotalUs[2]);
		}
		HealthJson += TEXT("}");

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

		FString Prom = FString::Printf(
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

		// Phase 28G: append pipeline latency metrics
		if (Impl->LatencyTracker)
		{
			auto P = Impl->LatencyTracker->ComputePercentiles();
			Prom += FString::Printf(
				TEXT("# HELP camsim_latency_readback_us Readback latency microseconds\n"
				     "# TYPE camsim_latency_readback_us gauge\n"
				     "camsim_latency_readback_us{quantile=\"0.5\"} %.0f\n"
				     "camsim_latency_readback_us{quantile=\"0.95\"} %.0f\n"
				     "camsim_latency_readback_us{quantile=\"0.99\"} %.0f\n"
				     "# HELP camsim_latency_sensor_us Sensor pipeline latency microseconds\n"
				     "# TYPE camsim_latency_sensor_us gauge\n"
				     "camsim_latency_sensor_us{quantile=\"0.5\"} %.0f\n"
				     "camsim_latency_sensor_us{quantile=\"0.95\"} %.0f\n"
				     "camsim_latency_sensor_us{quantile=\"0.99\"} %.0f\n"
				     "# HELP camsim_latency_encode_us Encode latency microseconds\n"
				     "# TYPE camsim_latency_encode_us gauge\n"
				     "camsim_latency_encode_us{quantile=\"0.5\"} %.0f\n"
				     "camsim_latency_encode_us{quantile=\"0.95\"} %.0f\n"
				     "camsim_latency_encode_us{quantile=\"0.99\"} %.0f\n"
				     "# HELP camsim_latency_total_us Total pipeline latency microseconds\n"
				     "# TYPE camsim_latency_total_us gauge\n"
				     "camsim_latency_total_us{quantile=\"0.5\"} %.0f\n"
				     "camsim_latency_total_us{quantile=\"0.95\"} %.0f\n"
				     "camsim_latency_total_us{quantile=\"0.99\"} %.0f\n"),
				P.ReadbackUs[0], P.ReadbackUs[1], P.ReadbackUs[2],
				P.SensorUs[0], P.SensorUs[1], P.SensorUs[2],
				P.EncodeUs[0], P.EncodeUs[1], P.EncodeUs[2],
				P.TotalUs[0], P.TotalUs[1], P.TotalUs[2]);
		}

		if (!FFileHelper::SaveStringToFile(Prom, *Config.PrometheusMetricsPath))
		{
			UE_LOG(LogCamSim, Warning, TEXT("UCamSimSubsystem: failed to write prometheus metrics"));
		}
		Impl->PrometheusLastTick = Impl->FrameCntr;
	}

	// Phase 28B: flush structured log queue
	if (Impl->JsonLogger)
	{
		Impl->JsonLogger->Flush();
	}

	++Impl->FrameCntr;
}
