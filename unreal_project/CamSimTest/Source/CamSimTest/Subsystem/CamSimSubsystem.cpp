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
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

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

	UE_LOG(LogCamSim, Log, TEXT("UCamSimSubsystem: initializing"));

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
	constexpr uint32 WatchdogInterval = 150;
	if (VideoEncoder && VideoEncoder->IsOpen() && FrameCntr > WatchdogInterval)
	{
		if ((FrameCntr - WatchdogLastCheckTick) >= WatchdogInterval)
		{
			const uint64 CurrentSuccess = VideoEncoder->GetSuccessfulFrameCount();
			if (CurrentSuccess == WatchdogLastSuccessFrame)
			{
				UE_LOG(LogCamSim, Warning,
					TEXT("UCamSimSubsystem: encoder watchdog — no frames written in %u ticks, reconnecting"),
					WatchdogInterval);
				VideoEncoder->Close();
				if (!VideoEncoder->Open())
				{
					UE_LOG(LogCamSim, Error, TEXT("UCamSimSubsystem: encoder watchdog — reopen failed"));
				}
			}
			WatchdogLastSuccessFrame = VideoEncoder->GetSuccessfulFrameCount();
			WatchdogLastCheckTick    = FrameCntr;
		}
	}

	// Write Docker HEALTHCHECK file every 90 ticks (~3s at 30fps)
	if (FrameCntr % 90 == 0)
	{
		const FString HealthPath = FPaths::Combine(FPlatformProcess::BaseDir(), TEXT("camsim_health"));
		FFileHelper::SaveStringToFile(FString::FromInt(static_cast<int32>(FrameCntr)), *HealthPath);
	}

	++FrameCntr;
}
