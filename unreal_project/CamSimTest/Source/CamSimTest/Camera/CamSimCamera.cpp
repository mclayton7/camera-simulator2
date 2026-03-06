// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimCamera.h"
#include "Camera/CamSimGimbalComponent.h"
#include "Camera/CamSimSensorComponent.h"
#include "Camera/CamSimPixelConvert.h"
#include "CamSimTest.h"
#include "Subsystem/CamSimSubsystem.h"
#include "CIGI/CigiReceiver.h"
#include "Encoder/VideoEncoder.h"
#include "Sensor/SensorPostProcess.h"  // FSensorPostProcess concrete type

#include "Encoder/IFrameSink.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/GameInstance.h"
#include "TextureResource.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "DynamicRHI.h"
#include "PixelFormat.h"
#include "Async/Async.h"

// Cesium
#include "CesiumGlobeAnchorComponent.h"
#include "CesiumCameraManager.h"
#include "CesiumCamera.h"
#include "CesiumGeoreference.h"
#include "Cesium3DTileset.h"
#include "EngineUtils.h"

// -------------------------------------------------------------------------
// Stats
// -------------------------------------------------------------------------

DECLARE_STATS_GROUP(TEXT("CamSim"), STATGROUP_CamSim, STATCAT_Advanced)
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Frames Dropped"), STAT_CamSimDropped, STATGROUP_CamSim)
DECLARE_CYCLE_STAT(TEXT("Encode Latency"),  STAT_CamSimEncode,   STATGROUP_CamSim)

// -------------------------------------------------------------------------
// Constructor
// -------------------------------------------------------------------------

ACamSimCamera::ACamSimCamera()
{
	PrimaryActorTick.bCanEverTick = true;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	GlobeAnchor = CreateDefaultSubobject<UCesiumGlobeAnchorComponent>(TEXT("GlobeAnchor"));

	SceneCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("SceneCapture"));
	SceneCapture->SetupAttachment(Root);

	GimbalComp = CreateDefaultSubobject<UCamSimGimbalComponent>(TEXT("GimbalComp"));
	SensorComp = CreateDefaultSubobject<UCamSimSensorComponent>(TEXT("SensorComp"));

	// Manual capture only — we drive it from Tick()
	SceneCapture->bCaptureEveryFrame   = false;
	SceneCapture->bCaptureOnMovement   = false;
	SceneCapture->CaptureSource        = SCS_FinalColorLDR;
	SceneCapture->bAlwaysPersistRenderingState = true;
}

// -------------------------------------------------------------------------
// BeginPlay
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

	const FCamSimConfig& Cfg = Subsystem->GetConfig();

	// Log the RHI backend — critical for diagnosing byte-order differences
	// between Metal (macOS, native BGRA) and Vulkan (Linux, may deliver RGBA).
	FString RHIName = GDynamicRHI ? GDynamicRHI->GetName() : TEXT("Unknown");
	UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: RHI=%s  Platform=%s"),
		*RHIName, ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName()));

	// Create render target
	RenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("CamSimRT"));
	RenderTarget->InitCustomFormat(
		Cfg.CaptureWidth, Cfg.CaptureHeight,
		PF_B8G8R8A8,
		/*bInForceLinearGamma=*/false);
	RenderTarget->UpdateResource();

	SceneCapture->TextureTarget = RenderTarget;
	SceneCapture->FOVAngle = Cfg.HFovDeg;

	// Move camera to configured start position
	if (GlobeAnchor)
	{
		GlobeAnchor->MoveToLongitudeLatitudeHeight(
			FVector(Cfg.StartLongitude, Cfg.StartLatitude, Cfg.StartAltitude));
		SetActorRotation(FRotator(Cfg.StartPitch, Cfg.StartYaw, Cfg.StartRoll));

		// Seed telemetry with start position
		CurrentTelemetry.Latitude  = Cfg.StartLatitude;
		CurrentTelemetry.Longitude = Cfg.StartLongitude;
		CurrentTelemetry.Altitude  = Cfg.StartAltitude;
		CurrentTelemetry.Yaw       = Cfg.StartYaw;
		CurrentTelemetry.Pitch     = Cfg.StartPitch;
		CurrentTelemetry.Roll      = Cfg.StartRoll;
		CurrentTelemetry.HFovDeg   = Cfg.HFovDeg;

		UE_LOG(LogCamSim, Log,
			TEXT("ACamSimCamera: start position lat=%.4f lon=%.4f alt=%.0fm yaw=%.0f pitch=%.0f"),
			Cfg.StartLatitude, Cfg.StartLongitude, Cfg.StartAltitude,
			Cfg.StartYaw, Cfg.StartPitch);
	}

	// Register with Cesium's camera manager so tiles stream for our viewpoint
	ACesiumCameraManager* CamMgr = ACesiumCameraManager::GetDefaultCameraManager(this);
	if (CamMgr)
	{
		const float PreloadFov = FMath::Clamp(Cfg.HFovDeg * Cfg.TilePreloadFovScale, Cfg.HFovDeg, 179.0f);
		FCesiumCamera InitCam(
			FVector2D(Cfg.CaptureWidth, Cfg.CaptureHeight),
			GetActorLocation(),
			GetActorRotation(),
			PreloadFov);
		CesiumCameraId = CamMgr->AddCamera(InitCam);
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: registered with CesiumCameraManager (id=%d, preload FOV=%.0f)"),
			CesiumCameraId, PreloadFov);
	}

	// Tune Cesium3DTileset actors for smoother streaming
	for (TActorIterator<ACesium3DTileset> It(GetWorld()); It; ++It)
	{
		It->MaximumSimultaneousTileLoads = Cfg.MaxSimultaneousTileLoads;
		It->PreloadAncestors = true;
		It->PreloadSiblings  = true;
		It->ForbidHoles      = true;
		It->LoadingDescendantLimit = 40;
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: tuned tileset '%s' (maxLoads=%d, ForbidHoles=true)"),
			*It->GetName(), Cfg.MaxSimultaneousTileLoads);
	}

	// Initialize CPU-side sensor post-processing pipeline (Phase 11)
	// Concrete FSensorPostProcess is stored behind the IPixelPipeline interface
	// so that a NullPixelPipeline or alternate waveband implementation can
	// be substituted without touching the camera actor.
	auto* Pipeline = new FSensorPostProcess();
	Pipeline->Initialize(Cfg.CaptureWidth, Cfg.CaptureHeight, Cfg.SensorModeConfigs);
	SensorFX.Reset(Pipeline);

	// Allocate async GPU readback helper (non-blocking DMA: EnqueueCopy → IsReady → Lock)
	GPUReadback = new FRHIGPUTextureReadback(TEXT("CamSimReadback"));

	UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: ready (%dx%d @ %.0ffps)"),
		Cfg.CaptureWidth, Cfg.CaptureHeight, Cfg.FrameRate);
}

// -------------------------------------------------------------------------
// EndPlay
// -------------------------------------------------------------------------

void ACamSimCamera::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (CesiumCameraId >= 0)
	{
		ACesiumCameraManager* CamMgr = ACesiumCameraManager::GetDefaultCameraManager(this);
		if (CamMgr)
		{
			CamMgr->RemoveCamera(CesiumCameraId);
		}
		CesiumCameraId = -1;
	}

	// Free the GPU readback object — must happen before the render target is destroyed
	if (GPUReadback)
	{
		delete GPUReadback;
		GPUReadback = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}

// -------------------------------------------------------------------------
// Tick – game thread
// -------------------------------------------------------------------------

void ACamSimCamera::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!Subsystem) return;

	// Heartbeat: log every 150 ticks (~5 s at 30 fps) so we can confirm
	// the tick is running and see whether the encoder is stuck busy.
	static uint64 TickCount = 0;
	if (++TickCount % 150 == 0)
	{
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: tick=%llu busy=%d readback=%d sensor=%d frames_encoded=%llu dropped=%llu"),
			TickCount, (int)(bool)bEncoderBusy, (int)(bool)bReadbackPending,
			(int)(SensorComp && SensorComp->IsOn()), FrameIndex, (uint64)DroppedFrameCount);
	}

	// Apply pending CIGI state first (CIGI queue drain runs every tick,
	// even if we skip capture, so the platform pose stays current)
	ApplyCigiState(DeltaTime);

	// -----------------------------------------------------------------------
	// State B — GPU readback pending: poll render thread for DMA completion.
	// We enqueue a render command each tick that checks IsReady(); when the
	// GPU finishes the DMA, it copies pixels and dispatches the encode task.
	// -----------------------------------------------------------------------
	if (bReadbackPending)
	{
		const uint64           WaitFrame     = PendingFrameIndex;
		const FCamSimTelemetry WaitTelemetry = PendingTelemetry;
		const FCamSimConfig&   Cfg           = Subsystem->GetConfig();
		const bool            bForceSwap     = Cfg.bSwapRBReadback;
		const FCamSimConfig::EReadbackFormat DesiredFormat = Cfg.ReadbackFormat;
		UTextureRenderTarget2D* RT            = RenderTarget;
		FRHIGPUTextureReadback* Readback      = GPUReadback;

		ENQUEUE_RENDER_COMMAND(CamSimPollReadback)(
			[this, WaitFrame, WaitTelemetry, bForceSwap, DesiredFormat, RT, Readback](FRHICommandListImmediate& RHICmdList)
		{
			// Guard against multiple poll commands queued for the same readback:
			// IsReady() returns true persistently once the GPU fence fires, so if several
			// CamSimPollReadback commands accumulated while the GPU was busy, all of them
			// would see IsReady()=true and each dispatch EncodeFrame with the same FrameIdx,
			// producing non-monotonic DTS in the muxer.  bReadbackClaimed ensures only the
			// first one proceeds; it is reset by CamSimEnqueueReadback each new capture.
			if (bReadbackClaimed) return;
			if (!Readback || !Readback->IsReady()) return;  // GPU still transferring — try next tick
			bReadbackClaimed = true;

			int32 RowPitch = 0;
			void* RawData = Readback->Lock(RowPitch);

			if (RawData && RT)
			{
				const int32 W = RT->SizeX;
				const int32 H = RT->SizeY;

				const EPixelFormat PixelFormat = RT->GetFormat();
				const bool bIsBgra = (PixelFormat == PF_B8G8R8A8);
				const bool bIsRgba = (PixelFormat == PF_R8G8B8A8);
				if (!bIsBgra && !bIsRgba)
				{
					UE_LOG(LogCamSim, Warning,
						TEXT("CamSimReadback frame %llu: unsupported pixel format %s"),
						WaitFrame, GetPixelFormatString(PixelFormat));
					Readback->Unlock();
					bReadbackPending = false;
					bEncoderBusy     = false;
					return;
				}

				TArray<FColor> Pixels;
				Pixels.SetNumUninitialized(W * H);
				CamSimConvertReadbackPixels(RawData, RowPitch, W, H,
					PixelFormat, DesiredFormat, bForceSwap, Pixels, WaitFrame);

				Readback->Unlock();
				bReadbackPending = false;

				SubmitFrameToEncoder(MoveTemp(Pixels), WaitTelemetry, WaitFrame);
			}
			else
			{
				Readback->Unlock();
				bReadbackPending = false;
				bEncoderBusy     = false;
				UE_LOG(LogCamSim, Warning, TEXT("CamSimPollReadback frame %llu: lock returned null"), WaitFrame);
			}
		});

		// While readback is in flight we do not start a new capture
		return;
	}

	// -----------------------------------------------------------------------
	// Busy check: the encode task from the previous readback is still running.
	// -----------------------------------------------------------------------
	if (bEncoderBusy)
	{
		++DroppedFrameCount;
		INC_DWORD_STAT(STAT_CamSimDropped);
		UE_LOG(LogCamSim, Verbose, TEXT("ACamSimCamera: encoder busy, skipping frame %llu (total dropped=%llu)"),
			FrameIndex, (uint64)DroppedFrameCount);
		return;
	}

	// Sensor off → no output (encoder idle, stream stalls — host will re-enable)
	if (SensorComp && !SensorComp->IsOn()) return;

	// -----------------------------------------------------------------------
	// State A — idle: trigger a new scene capture and enqueue async readback.
	// -----------------------------------------------------------------------
	CaptureAndEncode();
}

// -------------------------------------------------------------------------
// ApplyCigiState – consume CIGI queues on game thread
// -------------------------------------------------------------------------

void ACamSimCamera::ApplyCigiState(float DeltaTime)
{
	FCigiReceiver* Receiver = Subsystem->GetCigiReceiver();
	if (!Receiver) return;

	const FCamSimConfig& Cfg = Subsystem->GetConfig();

	// -----------------------------------------------------------------------
	// Camera Entity Control (opcode 2) → platform pose
	// -----------------------------------------------------------------------
	{
		FCigiEntityState EntityState;
		bool bGotState = false;
		while (Receiver->DequeueCameraEntityState(EntityState))
		{
			bGotState = true;
		}

		if (bGotState && GlobeAnchor)
		{
			if (FMath::IsNaN(EntityState.Latitude) || FMath::IsNaN(EntityState.Longitude) ||
				FMath::IsNaN(EntityState.Altitude) || FMath::IsNaN(EntityState.Yaw) ||
				FMath::IsNaN(EntityState.Pitch)    || FMath::IsNaN(EntityState.Roll))
			{
				UE_LOG(LogCamSim, Warning,
					TEXT("ACamSimCamera: CIGI entity state contains NaN - skipping"));
				return;
			}

			UE_LOG(LogCamSim, Verbose,
				TEXT("ACamSimCamera: CIGI -> lat=%.6f lon=%.6f alt=%.1f yaw=%.1f pitch=%.1f roll=%.1f"),
				EntityState.Latitude, EntityState.Longitude, EntityState.Altitude,
				EntityState.Yaw, EntityState.Pitch, EntityState.Roll);

			GlobeAnchor->MoveToLongitudeLatitudeHeight(
				FVector(EntityState.Longitude, EntityState.Latitude, EntityState.Altitude));
			SetActorRotation(FRotator(EntityState.Pitch, EntityState.Yaw, EntityState.Roll));

			FVector ActorScale = GetActorScale3D();
			if (ActorScale.ContainsNaN())
			{
				SetActorScale3D(FVector::OneVector);
			}

			CurrentTelemetry.Latitude  = EntityState.Latitude;
			CurrentTelemetry.Longitude = EntityState.Longitude;
			CurrentTelemetry.Altitude  = EntityState.Altitude;
			CurrentTelemetry.Yaw       = EntityState.Yaw;
			CurrentTelemetry.Pitch     = EntityState.Pitch;
			CurrentTelemetry.Roll      = EntityState.Roll;
			CurrentTelemetry.HFovDeg   = SceneCapture->FOVAngle;
			CurrentTelemetry.VFovDeg   = SceneCapture->FOVAngle *
				static_cast<float>(Cfg.CaptureHeight) /
				static_cast<float>(Cfg.CaptureWidth);

			UpdateCesiumCamera();
		}
	}

	// -----------------------------------------------------------------------
	// View Definition (opcode 20) → apply HFOV
	// -----------------------------------------------------------------------
	{
		FCigiViewDefinition ViewDef;
		while (Receiver->DequeueViewDefinition(ViewDef))
		{
			const float NewHFov = FMath::Clamp(ViewDef.HFovDeg(), 1.0f, 179.0f);
			if (SceneCapture && NewHFov != SceneCapture->FOVAngle)
			{
				SceneCapture->FOVAngle = NewHFov;
				UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: ViewDef -> HFOV=%.1f°"), NewHFov);
			}
		}
	}

	// -----------------------------------------------------------------------
	// Sensor Control (opcode 17) — delegated to UCamSimSensorComponent
	// -----------------------------------------------------------------------
	if (SensorComp)
	{
		SensorComp->TickSensor(Receiver, Cfg, SceneCapture);
	}

	// -----------------------------------------------------------------------
	// View Control (opcode 16) + Camera ArtPart → delegated to UCamSimGimbalComponent
	// -----------------------------------------------------------------------
	if (GimbalComp)
	{
		GimbalComp->TickGimbal(DeltaTime, Receiver, Cfg);

		// Apply result to the scene capture's relative rotation
		if (SceneCapture)
		{
			SceneCapture->SetRelativeRotation(GimbalComp->GetGimbalRelativeRotation());
		}

		// Sync into telemetry for KLV output
		CurrentTelemetry.GimbalYaw   = GimbalComp->GetGimbalYaw();
		CurrentTelemetry.GimbalPitch = GimbalComp->GetGimbalPitch();
		CurrentTelemetry.GimbalRoll  = GimbalComp->GetGimbalRoll();
	}

	// Compute slant range and frame centre from current pose + gimbal
	ComputeGeometricLOS();
}

// -------------------------------------------------------------------------
// ComputeGeometricLOS – slant range and frame center from platform + gimbal
// -------------------------------------------------------------------------

void ACamSimCamera::ComputeGeometricLOS()
{
	if (!SceneCapture) return;

	// -----------------------------------------------------------------------
	// Primary: UE line trace along sensor boresight (accounts for real terrain)
	// -----------------------------------------------------------------------
	UWorld* World = GetWorld();
	if (World)
	{
		const FVector RayStart = SceneCapture->GetComponentLocation();
		const FVector RayDir   = SceneCapture->GetForwardVector();

		// 5 000 km max range covers all reasonable ISR sensor geometries
		const FVector RayEnd   = RayStart + RayDir * 500'000'000.0f;  // 5000 km in cm

		FHitResult Hit;
		FCollisionQueryParams Params(NAME_None, /*bTraceComplex=*/false);
		Params.AddIgnoredActor(this);

		if (World->LineTraceSingleByChannel(Hit, RayStart, RayEnd, ECC_Visibility, Params))
		{
			// UE units are centimetres — convert to metres
			CurrentTelemetry.SlantRangeM = static_cast<double>(Hit.Distance) / 100.0;

			// Convert UE world-space hit point to geodetic via Cesium georeference
			ACesiumGeoreference* GeoRef = ACesiumGeoreference::GetDefaultGeoreference(World);
			if (GeoRef)
			{
				// Returns FVector(Longitude, Latitude, HeightM)
				const FVector LLH = GeoRef->TransformUnrealPositionToLongitudeLatitudeHeight(Hit.Location);
				CurrentTelemetry.FrameCenterLon = LLH.X;
				CurrentTelemetry.FrameCenterLat = LLH.Y;
			}
			return;
		}
	}

	// -----------------------------------------------------------------------
	// Fallback: flat-earth approximation (sensor above horizon or no geometry)
	// -----------------------------------------------------------------------
	const float GimbalPitchVal = GimbalComp ? GimbalComp->GetGimbalPitch() : 0.0f;
	const float GimbalYawVal   = GimbalComp ? GimbalComp->GetGimbalYaw()   : 0.0f;

	const float WorldPitchDeg  = CurrentTelemetry.Pitch + GimbalPitchVal;
	const float WorldYawDeg    = CurrentTelemetry.Yaw   + GimbalYawVal;
	const float DepressionDeg  = -WorldPitchDeg;

	if (DepressionDeg <= 0.0f)
	{
		CurrentTelemetry.SlantRangeM = 0.0;
		return;
	}

	const double AltM       = CurrentTelemetry.Altitude;
	const double DepressRad = FMath::DegreesToRadians(static_cast<double>(DepressionDeg));
	const double AzimuthRad = FMath::DegreesToRadians(static_cast<double>(WorldYawDeg));

	CurrentTelemetry.SlantRangeM = AltM / FMath::Sin(DepressRad);
	const double GroundM = AltM / FMath::Tan(DepressRad);

	const double LatRad = FMath::DegreesToRadians(CurrentTelemetry.Latitude);
	CurrentTelemetry.FrameCenterLat = CurrentTelemetry.Latitude +
		(GroundM * FMath::Cos(AzimuthRad)) / 111320.0;
	CurrentTelemetry.FrameCenterLon = CurrentTelemetry.Longitude +
		(GroundM * FMath::Sin(AzimuthRad)) / (111320.0 * FMath::Cos(LatRad));
}

// -------------------------------------------------------------------------
// UpdateCesiumCamera – keep Cesium tile streaming in sync with our pose
// -------------------------------------------------------------------------

void ACamSimCamera::UpdateCesiumCamera()
{
	if (CesiumCameraId < 0 || !Subsystem) return;

	ACesiumCameraManager* CamMgr = ACesiumCameraManager::GetDefaultCameraManager(this);
	if (!CamMgr) return;

	const FCamSimConfig& Cfg = Subsystem->GetConfig();
	// Inflate FOV so Cesium preloads tiles beyond the visible frustum
	const float PreloadFov = FMath::Clamp(Cfg.HFovDeg * Cfg.TilePreloadFovScale, Cfg.HFovDeg, 179.0f);
	FCesiumCamera Cam(
		FVector2D(Cfg.CaptureWidth, Cfg.CaptureHeight),
		SceneCapture->GetComponentLocation(),
		SceneCapture->GetComponentRotation(),
		PreloadFov);

	CamMgr->UpdateCamera(CesiumCameraId, Cam);
}

// -------------------------------------------------------------------------
// CaptureAndEncode – State A: trigger GPU capture + enqueue async readback
// -------------------------------------------------------------------------
// State B (polling completion) runs in Tick() via the bReadbackPending branch.
// The two states naturally double-buffer: frame N+1 renders on the game thread
// while the render thread completes frame N's DMA and the encode task runs.

void ACamSimCamera::CaptureAndEncode()
{
	if (!RenderTarget || !SceneCapture || !GPUReadback) return;

	// Snapshot telemetry immediately before capture so KLV timestamp is accurate
	CurrentTelemetry.TimestampUs =
		static_cast<uint64>(FPlatformTime::Seconds() * 1'000'000.0);

	SceneCapture->CaptureScene();

	PendingFrameIndex = FrameIndex++;
	PendingTelemetry  = CurrentTelemetry;

	// Flag both readback and encoder as busy; bEncoderBusy is cleared only
	// when the async encode task finishes (not when the readback completes).
	bReadbackPending = true;
	bEncoderBusy     = true;

	// Enqueue the async GPU→CPU DMA on the render thread (returns immediately)
	UTextureRenderTarget2D*  RT       = RenderTarget;
	FRHIGPUTextureReadback*  Readback = GPUReadback;

	ENQUEUE_RENDER_COMMAND(CamSimEnqueueReadback)(
		[this, RT, Readback](FRHICommandListImmediate& RHICmdList)
	{
		// Reset the claim flag so the first CamSimPollReadback that fires can claim it.
		// Subsequent poll commands from the same session will see bReadbackClaimed=true and bail.
		bReadbackClaimed = false;

		FTextureRenderTargetResource* Resource = RT->GetRenderTargetResource();
		if (!Resource) return;
		// Non-blocking: kicks off DMA; IsReady() polled in subsequent Tick(s)
		Readback->EnqueueCopy(RHICmdList, Resource->GetRenderTargetTexture());
	});
}

// -------------------------------------------------------------------------
// SubmitFrameToEncoder – called from render thread, dispatches async task
// -------------------------------------------------------------------------

void ACamSimCamera::SubmitFrameToEncoder(
	TArray<FColor> PixelData, FCamSimTelemetry Telemetry, uint64 FrameIdx)
{
	IFrameSink* Encoder = Subsystem ? Subsystem->GetVideoEncoder() : nullptr;
	if (!Encoder)
	{
		bEncoderBusy = false;
		return;
	}

	// Capture sensor state for the lambda (game-thread variables, safe to read here).
	const ESensorMode  Mode     = SensorComp ? SensorComp->GetMode()     : ESensorMode::EO;
	const uint8        Polarity = SensorComp ? SensorComp->GetPolarity() : 0;
	IPixelPipeline*    FX       = SensorFX.Get();

	// Move pixel buffer into async task to avoid copy
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
		[this, Encoder, FX, Mode, Polarity, Pixels = MoveTemp(PixelData), Telemetry, FrameIdx]() mutable
	{
		SCOPE_CYCLE_COUNTER(STAT_CamSimEncode);
		// Apply CPU-side sensor post-processing before encode
		if (FX)
		{
			FX->Process(Pixels, Mode, Polarity, Telemetry, FrameIdx);
		}
		Encoder->EncodeFrame(Pixels, Telemetry, FrameIdx);
		bEncoderBusy = false;
	});
}
