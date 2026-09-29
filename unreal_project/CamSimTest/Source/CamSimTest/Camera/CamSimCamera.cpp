// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimCamera.h"
#include "Camera/CamSimGimbalComponent.h"
#include "Camera/CamSimSensorComponent.h"
#include "CamSimTest.h"
#include "CIGI/CigiReceiver.h"
#include "Config/CamSimConfig.h"
#include "Diagnostics/PipelineLatencyTracker.h"
#include "Environment/CamSimEnvironment.h"
#include "Geospatial/CamSimGeospatialProvider.h"
#include "Geospatial/CesiumTuning.h"
#include "Subsystem/CamSimSubsystem.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/PlayerController.h"
#include "UnrealEngine.h"  // FSystemResolution
#include "Camera/CamSimRenderPath.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h" // TActorIterator
#include "DynamicRHI.h"
#include "RenderTimer.h"      // GGameThreadTime, GRenderThreadTime, GRHIThreadTime
#include "Cesium3DTileset.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "CesiumGlobeAnchorComponent.h"
#include "CesiumOriginShiftComponent.h"

ACamSimCamera::ACamSimCamera()
{
	PrimaryActorTick.bCanEverTick = true;
	// Capture last: after FCamSimEntityManager (a tickable object, which runs
	// between TG_PostPhysics and TG_PostUpdateWork) has applied this frame's
	// CIGI entity states and attachments, so the image shows them this frame.
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	GlobeAnchor = CreateDefaultSubobject<UCesiumGlobeAnchorComponent>(TEXT("GlobeAnchor"));

	SceneCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("SceneCapture"));
	SceneCapture->SetupAttachment(Root);

	SensorCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("SensorCamera"));
	SensorCamera->SetupAttachment(SceneCapture);
	SensorCamera->bConstrainAspectRatio = false;
	SensorCamera->bUsePawnControlRotation = false;

	OriginShift = CreateDefaultSubobject<UCesiumOriginShiftComponent>(TEXT("OriginShift"));
	OriginShift->SetMode(ECesiumOriginShiftMode::Disabled);  // configured in BeginPlay

	GimbalComp  = CreateDefaultSubobject<UCamSimGimbalComponent>(TEXT("GimbalComp"));
	SensorComp  = CreateDefaultSubobject<UCamSimSensorComponent>(TEXT("SensorComp"));
	CaptureComp = CreateDefaultSubobject<UCamSimCaptureComponent>(TEXT("CaptureComp"));

	// Manual capture only — driven from Tick()
	SceneCapture->bCaptureEveryFrame   = false;
	SceneCapture->bCaptureOnMovement   = false;
	SceneCapture->CaptureSource        = SCS_FinalColorLDR;
	SceneCapture->bAlwaysPersistRenderingState = true;

	// FXAA: TSR's temporal history doesn't accumulate on off-screen captures
	// (persistent ghosting); FXAA is stateless.
	SceneCapture->ShowFlags.SetTemporalAA(false);
	SceneCapture->ShowFlags.SetAntiAliasing(true);
	SceneCapture->ShowFlags.SetDynamicShadows(true);  // 24A (VSM enabled in DefaultEngine.ini)
	SceneCapture->ShowFlags.SetMaterialNormal(true);  // 24C
}

// -------------------------------------------------------------------------
// BeginPlay / EndPlay
// -------------------------------------------------------------------------

void ACamSimCamera::BeginPlay()
{
	Super::BeginPlay();

	Subsystem = GetGameInstance()->GetSubsystem<UCamSimSubsystem>();
	if (!Subsystem)
	{
		UE_LOG(LogCamSim, Error, TEXT("ACamSimCamera: UCamSimSubsystem not found"));
		return;
	}
	Subsystem->RegisterCamera(this);

	// Environment (sun, fog, weather) also ticks in TG_PostUpdateWork: apply it
	// before this frame's capture. ACamSimEnvironment adds the same
	// prerequisite if it begins play after us.
	for (TActorIterator<ACamSimEnvironment> It(GetWorld()); It; ++It)
	{
		AddTickPrerequisiteActor(*It);
	}

	const FCamSimConfig& Cfg = Subsystem->GetConfig();
	UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: RHI=%s  Platform=%s"),
		GDynamicRHI ? GDynamicRHI->GetName() : TEXT("Unknown"), ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName()));

	Platform.Initialize(this, GlobeAnchor, Subsystem, &Telemetry, Cfg);
	Streaming.Initialize(this, Cfg);

	if (Cfg.Render.OriginShiftDistanceM > 0.0)
	{
		// ChangeCesiumGeoreference moves tilesets, so they must be Movable. The
		// sky (CesiumSunSky) and globe-anchored actors follow the georeference.
		for (TActorIterator<ACesium3DTileset> It(GetWorld()); It; ++It)
		{
			if (USceneComponent* TilesetRoot = It->GetRootComponent())
			{
				TilesetRoot->SetMobility(EComponentMobility::Movable);
			}
		}
		OriginShift->SetDistance(Cfg.Render.OriginShiftDistanceM * 100.0);  // m -> UE cm (compared to GetActorLocation)
		OriginShift->SetMode(ECesiumOriginShiftMode::ChangeCesiumGeoreference);
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: origin shift every %.0f m"), Cfg.Render.OriginShiftDistanceM);
	}

	// Tileset streaming parameters and the Cesium backend (ion server, terrain,
	// imagery). The tuning helper is shared with UCamSimSubsystem::HotReloadConfig.
	CamSim::Geospatial::ApplyCesiumTilesetTuning(GetWorld(), Cfg);
	Subsystem->StoreCesiumIonServer(ApplyCesiumBackendConfig(GetWorld(), Cfg.CesiumBackend));

	// The game viewport renders at the stream resolution (ROADMAP 3A).
	FSystemResolution::RequestResolutionChange(Cfg.CaptureWidth, Cfg.CaptureHeight, EWindowMode::Windowed);

	CaptureComp->Initialize(SceneCapture, Subsystem, Cfg);
	SetLatencyTracker(Subsystem->GetLatencyTracker());

	if (!Cfg.Operational.FrameStatsPath.IsEmpty() && FrameStats.Open(Cfg.Operational.FrameStatsPath))
	{
		ViewFamilyCounter = FSceneViewExtensions::NewExtension<FCamSimViewFamilyCounter>();
		LastStatsWallSec = FPlatformTime::Seconds();
	}

	UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: ready (%dx%d @ %.0ffps)"),
		Cfg.CaptureWidth, Cfg.CaptureHeight, Cfg.FrameRate);
}

void ACamSimCamera::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Streaming.Shutdown(this);
	CaptureComp->Shutdown();
	FrameStats.Close();
	ViewFamilyCounter.Reset();

	// Phase 27B — deregister so the health writer doesn't touch a dangling pointer.
	if (Subsystem)
	{
		Subsystem->RegisterCamera(nullptr);
	}
	Super::EndPlay(EndPlayReason);
}

void ACamSimCamera::SetLatencyTracker(FPipelineLatencyTracker* Tracker)
{
	LatencyTracker = Tracker;
	CaptureComp->SetLatencyTracker(Tracker);
}

// -------------------------------------------------------------------------
// Tick – game thread
// -------------------------------------------------------------------------

void ACamSimCamera::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!Subsystem) return;

	RecordFrameStats();  // stats for the frame that just finished
	bCameraCutThisFrame = false;

	++TickCount;
	EmitHeartbeatIfDue();
	PollHotReloadConfig(DeltaTime);

	if (LatencyTracker) LatencyTracker->Mark(EPipelineStage::GameTickStart);

	const FCamSimConfig& Cfg = Subsystem->GetConfig();

	// Platform pose. Normally FCamSimEntityManager already applied it this
	// frame, in order with the other entities; this covers running without one.
	Platform.ApplyHostPlatformState();
	Platform.FollowAttachParent();

	ApplyCigiViewState(DeltaTime);

	Streaming.UpdateLevelOfDetail(DeltaTime, GimbalComp->GetGimbalYaw(), GimbalComp->GetGimbalPitch(),
		SceneCapture->FOVAngle, Cfg, Subsystem->GetCachedTilesets());

	// Telemetry that depends on the final pose, gimbal and FOV of this frame.
	Telemetry.SetFieldOfView(SceneCapture->FOVAngle,
		SceneCapture->FOVAngle * static_cast<float>(Cfg.CaptureHeight) / static_cast<float>(Cfg.CaptureWidth));
	Telemetry.ReadEnvironment(GetWorld());

	// Stream tiles for the true frustum of this frame, then find the frame centre.
	Streaming.UpdateCameras(this, *SceneCapture, Cfg);
	Telemetry.UpdateFrameCenter(GetWorld(), *SceneCapture, this, Subsystem->GetGeospatialProvider());

	UpdateAutoFocus();
	ApplyPrimaryView();
	UpdateCameraCut();
	// The engine updated the player camera before this tick group; render
	// this frame's gimbal/FOV/post-process, the pose the KLV reports.
	CamSimRender::RefreshPlayerView(GetWorld()->GetFirstPlayerController(), DeltaTime);

	if (LatencyTracker) LatencyTracker->Mark(EPipelineStage::CigiDequeue);

	// ROADMAP 3B: sensor AE / AGC and graph parameters (no-op without the sensor graph).
	CaptureComp->UpdateSensorParams(SensorComp->GetMode(), SensorComp->GetPolarity(), bCameraCutThisFrame,
		SceneCapture->FOVAngle, Cfg);
	CaptureComp->Poll();

	if (!SensorComp->IsOn()) return;
	if (!Streaming.UpdateTerrainGate(Telemetry.Get().Latitude, Telemetry.Get().Longitude, Subsystem->GetCachedTilesets())) return;
	if (CaptureComp->ShouldSkipFrameForDecimation(Cfg)) return;

	if (CaptureComp->IsReadyForCapture())
	{
		CaptureAndEncode();
	}
	else
	{
		CaptureComp->NoteCaptureSkipped();  // all readback slots busy: the sensor/encoder is behind
	}
}

void ACamSimCamera::RecordFrameStats()
{
	if (!FrameStats.IsOpen()) return;

	const double NowSec = FPlatformTime::Seconds();
	const double MsPerCycle = FPlatformTime::GetSecondsPerCycle() * 1000.0;

	FCamSimFrameStatsSample S;
	S.UtcSeconds    = (FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalSeconds();
	S.WallMs        = (NowSec - LastStatsWallSec) * 1000.0;
	S.GameMs        = GGameThreadTime   * MsPerCycle;
	S.RenderMs      = GRenderThreadTime * MsPerCycle;
	S.RhiMs         = GRHIThreadTime    * MsPerCycle;
	S.GpuMs         = RHIGetGPUFrameCycles(0) * MsPerCycle;
	S.FramesEmitted = CaptureComp->GetFramesCaptured();
	S.FramesDropped = CaptureComp->GetDroppedFrameCount();
	S.bCameraCut    = bCameraCutThisFrame;
	S.ViewFamilies  = ViewFamilyCounter ? ViewFamilyCounter->ConsumeCount() : 0;
	S.SensorGpuMs      = CaptureComp->GetSensorGpuMs();
	S.SensorGainEv     = CaptureComp->GetSensorGainEv();
	S.SceneMedianLog2  = CaptureComp->GetSceneMedianLog2();
	S.bHasSensorStats  = CaptureComp->HasSensorGraph();

	float MinLoad = 100.0f;
	double Sse = 0.0;
	for (const TWeakObjectPtr<ACesium3DTileset>& Weak : Subsystem->GetCachedTilesets())
	{
		if (ACesium3DTileset* T = Weak.Get())
		{
			MinLoad = FMath::Min(MinLoad, T->GetLoadProgress());  // already 0-100
			Sse = T->GetMaximumScreenSpaceError();
		}
	}
	S.MinLoadProgressPct = MinLoad;
	S.Sse = Sse;

	FrameStats.Record(S);
	LastStatsWallSec = NowSec;
}

void ACamSimCamera::ApplyPrimaryView()
{
	// SceneCapture holds pose (via attachment), FOV and post-process; mirror them.
	SensorCamera->SetFieldOfView(SceneCapture->FOVAngle);
	SensorCamera->PostProcessSettings = SceneCapture->PostProcessSettings;
	SensorCamera->PostProcessBlendWeight = 1.0f;

	if (!bViewTargetApplied)
	{
		if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			PC->SetViewTarget(this);
			bViewTargetApplied = true;
			UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: sensor is the primary view (%s)"), *PC->GetName());
		}
	}
}

void ACamSimCamera::UpdateCameraCut()
{
	const FCamSimConfig& Cfg = Subsystem->GetConfig();
	const FVector Loc = SceneCapture->GetComponentLocation();
	const FQuat   Rot = SceneCapture->GetComponentQuat();
	if (bHasPrevView && CamSimRender::ShouldCutCamera(PrevViewLocCm, PrevViewRot, Loc, Rot,
		Cfg.Render.CameraCutDistanceM, Cfg.Render.CameraCutAngleDeg))
	{
		bCameraCutThisFrame = true;
		if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			if (PC->PlayerCameraManager) PC->PlayerCameraManager->SetGameCameraCutThisFrame();
		}
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: camera cut (moved %.0f m)"), FVector::Dist(PrevViewLocCm, Loc) / 100.0);
	}
	PrevViewLocCm = Loc;
	PrevViewRot   = Rot;
	bHasPrevView  = true;
}

void ACamSimCamera::CaptureAndEncode()
{
	// Snapshot (and UTC-stamp) the telemetry immediately before the capture.
	CaptureComp->Capture(Telemetry.Snapshot());
}

// -------------------------------------------------------------------------
// CIGI view state
// -------------------------------------------------------------------------

void ACamSimCamera::ApplyCigiViewState(float DeltaTime)
{
	FCigiReceiver* Receiver = Subsystem->GetCigiReceiver();
	if (!Receiver) return;
	const FCamSimConfig& Cfg = Subsystem->GetConfig();

	// Sensor Control (opcode 17): a gain that selects a new preset sets the FOV.
	SensorComp->TickSensor(Receiver, Cfg, SceneCapture);
	Telemetry.SetSensor(static_cast<uint8>(SensorComp->GetMode()), SensorComp->GetPolarity());

	// View Definition (opcode 21) → horizontal FOV. Applied after Sensor Control
	// so an explicit FOV wins when both arrive in the same host frame.
	FCigiViewDefinition ViewDef;
	while (Receiver->DequeueViewDefinition(ViewDef))
	{
		const float NewHFov = FMath::Clamp(ViewDef.HFovDeg(), 1.0f, 179.0f);
		if (NewHFov != SceneCapture->FOVAngle)
		{
			SceneCapture->FOVAngle = NewHFov;
			UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: ViewDef -> HFOV=%.1f°"), NewHFov);
		}
	}

	// View Control (opcode 16) + camera Art Part → gimbal; a View Control
	// naming an entity starts the first-person view (Phase 22G).
	GimbalComp->TickGimbal(DeltaTime, Receiver, Cfg);
	Platform.UpdateFirstPersonView(GimbalComp->GetLastViewControlEntityId());
	SceneCapture->SetRelativeRotation(GimbalComp->GetGimbalRelativeRotation());
	Telemetry.SetGimbal(GimbalComp->GetGimbalYaw(), GimbalComp->GetGimbalPitch(), GimbalComp->GetGimbalRoll());
}

void ACamSimCamera::UpdateAutoFocus()
{
	const FCamSimConfig::FOpticalRealismConfig& Opt = Subsystem->GetConfig().OpticalRealism;
	const double SlantRangeM = Telemetry.Get().SlantRangeM;
	if (Opt.bEnabled && Opt.bDepthOfField && Opt.FocalDistance <= 0.0f && SlantRangeM > 0.0)
	{
		SceneCapture->PostProcessSettings.bOverride_DepthOfFieldFocalDistance = true;
		SceneCapture->PostProcessSettings.DepthOfFieldFocalDistance = static_cast<float>(SlantRangeM * 100.0);  // m → cm
	}
}

// -------------------------------------------------------------------------
// Heartbeat and config hot reload
// -------------------------------------------------------------------------

void ACamSimCamera::EmitHeartbeatIfDue()
{
	// Every 150 ticks (~5 s at 30 fps): confirms the tick runs and shows
	// whether the pipeline is stuck.
	if ((TickCount % 150) != 0) return;

	const double NowSec = FPlatformTime::Seconds();
	const double WallDeltaSec = (LastHeartbeatWallSec > 0.0) ? (NowSec - LastHeartbeatWallSec) : 0.0;
	const double EffectiveFps = (WallDeltaSec > 0.0) ? (150.0 / WallDeltaSec) : 0.0;
	LastHeartbeatWallSec = NowSec;

	UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: tick=%llu busy=%d readback=%d sensor=%d frames_encoded=%llu dropped=%llu cap_idx=%d pending_idx=%d ready_polls=%d wall_dt=%.1fs fps=%.1f"),
		TickCount, (int)CaptureComp->IsSensorBusy(), (int)CaptureComp->IsReadbackInFlight(),
		(int)SensorComp->IsOn(), CaptureComp->GetFramesCaptured(), CaptureComp->GetDroppedFrameCount(),
		CaptureComp->GetCaptureTargetIndex(), CaptureComp->GetPendingReadbackTargetIndex(),
		FMath::Max(1, Subsystem->GetConfig().ReadbackReadyPolls), WallDeltaSec, EffectiveFps);

	FCamSimStreamingController::LogTilesetStats(Subsystem->GetCachedTilesets());
}

void ACamSimCamera::PollHotReloadConfig(float DeltaTime)
{
	const FCamSimConfig& Cfg = Subsystem->GetConfig();
	if (!Cfg.Performance.bHotReloadConfig) return;

	HotReloadAccumSec += DeltaTime;
	if (HotReloadAccumSec >= Cfg.Performance.HotReloadPollIntervalSec
	    && !bHotReloadStatInFlight.Load(EMemoryOrder::Relaxed))
	{
		HotReloadAccumSec = 0.0f;
		bHotReloadStatInFlight.Store(true, EMemoryOrder::Relaxed);
		const FString CfgPath = FCamSimConfig::GetConfigFilePath();
		AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, CfgPath]()
		{
			const FDateTime CurrMTime = IFileManager::Get().GetTimeStamp(*CfgPath);
			if (CurrMTime != FDateTime::MinValue() && CurrMTime != LastConfigMTime)
			{
				LastConfigMTime = CurrMTime;
				bHotReloadFileChanged.Store(true, EMemoryOrder::SequentiallyConsistent);
			}
			bHotReloadStatInFlight.Store(false, EMemoryOrder::Relaxed);
		});
	}

	// TAtomic<T>::Exchange is single-arg (seq_cst), matching the Store above.
	if (!bHotReloadFileChanged.Exchange(false))
	{
		return;
	}

	const FCamSimConfig OldCfg = Cfg;
	FCamSimConfig NewCfg = FCamSimConfig::Load();
	// Same gate as startup: a config Validate() rejects (or one that failed to parse) never goes live.
	const TArray<FString> ReloadErrors = FCamSimConfig::ValidateHotReload(OldCfg, NewCfg);
	if (ReloadErrors.Num() > 0)
	{
		for (const FString& Err : ReloadErrors)
		{
			UE_LOG(LogCamSim, Error, TEXT("HotReload: %s"), *Err);
		}
		UE_LOG(LogCamSim, Warning, TEXT("HotReload: %d config error(s) — keeping current config"), ReloadErrors.Num());
		return;
	}

	if (NewCfg.CigiPort != OldCfg.CigiPort)
		UE_LOG(LogCamSim, Warning, TEXT("HotReload: CIGI port change ignored (requires restart)"));
	if (NewCfg.MulticastAddr != OldCfg.MulticastAddr)
		UE_LOG(LogCamSim, Warning, TEXT("HotReload: multicast addr change ignored (requires restart)"));
	if (NewCfg.VideoCodec != OldCfg.VideoCodec)
		UE_LOG(LogCamSim, Warning, TEXT("HotReload: video codec change ignored (requires restart)"));
	if (NewCfg.MulticastPort != OldCfg.MulticastPort)
		UE_LOG(LogCamSim, Warning, TEXT("HotReload: multicast port change ignored (requires restart)"));
	if (NewCfg.Render.OriginShiftDistanceM != OldCfg.Render.OriginShiftDistanceM)
		UE_LOG(LogCamSim, Warning, TEXT("HotReload: origin_shift_distance_m change ignored (requires restart)"));
	if (NewCfg.CaptureWidth != OldCfg.CaptureWidth || NewCfg.CaptureHeight != OldCfg.CaptureHeight)
		UE_LOG(LogCamSim, Warning, TEXT("HotReload: capture_width / capture_height change ignored (requires restart)"));

	Subsystem->HotReloadConfig(NewCfg);
	UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: HotReload applied from %s"), *FCamSimConfig::GetConfigFilePath());
}
