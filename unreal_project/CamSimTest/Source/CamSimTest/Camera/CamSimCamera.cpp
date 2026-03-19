// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimCamera.h"
#include "Camera/CamSimGimbalComponent.h"
#include "Camera/CamSimSensorComponent.h"
#include "Camera/CamSimPixelConvert.h"
#include "CamSimTest.h"
#include "Subsystem/CamSimSubsystem.h"
#include "CIGI/CigiReceiver.h"
#include "Encoder/VideoEncoder.h"
#include "Encoder/EncoderThread.h"
#include "Sensor/SensorPostProcess.h"  // FSensorPostProcess concrete type
#include "Geospatial/CamSimGeospatialProvider.h"
#include "Environment/CamSimEnvironment.h"

#include "Encoder/IFrameSink.h"
#include "GroundTruth/FGroundTruthCollector.h"
#include "GroundTruth/FEntityProjection.h"
#include "Entity/CamSimEntityManager.h"
#include "Entity/CamSimEntity.h"
#include "DIS/DisEntityAdapter.h"
#include "EngineUtils.h" // TActorIterator
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
#include "HAL/FileManager.h"

// Phase 27A — GPU sensor post-process material loading
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Kismet/KismetMaterialLibrary.h"

// Cesium
#include "CesiumGlobeAnchorComponent.h"
#include "CesiumCameraManager.h"
#include "CesiumCamera.h"
#include "Cesium3DTileset.h"
#include <Cesium3DTilesSelection/Tileset.h>
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

	// Use FXAA for SceneCapture2D — TSR's temporal history does not accumulate
	// correctly on off-screen captures, producing persistent ghosting/blur.
	// FXAA is stateless and works correctly with bCaptureEveryFrame=false.
	SceneCapture->ShowFlags.SetTemporalAA(false);
	SceneCapture->ShowFlags.SetAntiAliasing(true);

	// 24A: Dynamic shadows between entities (VSM is enabled globally in DefaultEngine.ini)
	SceneCapture->ShowFlags.SetDynamicShadows(true);

	// 24C: Normal maps (ensure not disabled on SceneCapture)
	SceneCapture->ShowFlags.SetMaterialNormal(true);
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
	Subsystem->RegisterCamera(this);

	const FCamSimConfig& Cfg = Subsystem->GetConfig();

	bTrackFrameDrops_ = Cfg.Performance.bTrackFrameDropsByCategory;
	UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: FrameDropTracking=%s"),
		bTrackFrameDrops_ ? TEXT("enabled") : TEXT("disabled"));

	// Log the RHI backend for platform-specific diagnostics.
	FString RHIName = GDynamicRHI ? GDynamicRHI->GetName() : TEXT("Unknown");
	UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: RHI=%s  Platform=%s"),
		*RHIName, ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName()));

	// Create triple-buffer render targets: frame N+2 renders while N+1 reads back
	// and N encodes.  The extra target eliminates readback stalls (+3-5 fps).
	// Cost: +3.5 MB VRAM at 1280x720.
	RenderTargets.Reset();
	for (int32 Idx = 0; Idx < 3; ++Idx)
	{
		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
			this, *FString::Printf(TEXT("CamSimRT_%d"), Idx));
		if (!RT)
		{
			UE_LOG(LogCamSim, Error, TEXT("ACamSimCamera: failed to create render target %d"), Idx);
			return;
		}
		RT->InitCustomFormat(
			Cfg.CaptureWidth, Cfg.CaptureHeight,
			PF_B8G8R8A8,
			/*bInForceLinearGamma=*/false);
		RT->UpdateResource();
		RenderTargets.Add(RT);
	}

	CaptureTargetIndex = 0;
	PendingReadbackTargetIndex = INDEX_NONE;
	SceneCapture->TextureTarget = RenderTargets[CaptureTargetIndex];
	SceneCapture->FOVAngle = Cfg.HFovDeg;

	// Move camera to configured start position
	if (GlobeAnchor)
	{
		GlobeAnchor->MoveToLongitudeLatitudeHeight(
			FVector(Cfg.StartLongitude, Cfg.StartLatitude, Cfg.StartAltitude));
		SetActorScale3D(FVector::OneVector);
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

	// Phase 22G: Initialize FPS view from config
	FpsEntityId_   = static_cast<uint16>(FMath::Clamp(Cfg.FpsEntityId, 0, 65535));
	FpsEyeHeightM_ = Cfg.FpsEyeHeightM;
	if (FpsEntityId_ != 0)
	{
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: FPS mode -> entity %u (config)"), FpsEntityId_);
	}

	// Register with Cesium's camera manager so tiles stream for our viewpoint.
	// Two cameras: primary (actual FOV) drives accurate SSE/LOD for visible tiles,
	// prefetch (inflated FOV) preloads tiles outside the frustum for gimbal slews.
	ACesiumCameraManager* CamMgr = ACesiumCameraManager::GetDefaultCameraManager(this);
	if (CamMgr)
	{
		// Primary camera — actual render FOV drives tile LOD/SSE
		FCesiumCamera PrimaryCam(
			FVector2D(Cfg.CaptureWidth, Cfg.CaptureHeight),
			GetActorLocation(),
			GetActorRotation(),
			Cfg.HFovDeg);
		CesiumCameraId = CamMgr->AddCamera(PrimaryCam);

		// Prefetch camera — inflated FOV for anticipatory tile loading
		const float PreloadFov = FMath::Clamp(Cfg.HFovDeg * Cfg.TilePreloadFovScale, Cfg.HFovDeg, 179.0f);
		FCesiumCamera PrefetchCam(
			FVector2D(Cfg.CaptureWidth, Cfg.CaptureHeight),
			GetActorLocation(),
			GetActorRotation(),
			PreloadFov);
		CesiumPrefetchCameraId = CamMgr->AddCamera(PrefetchCam);

		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: registered with CesiumCameraManager (primary id=%d FOV=%.0f, prefetch id=%d FOV=%.0f)"),
			CesiumCameraId, Cfg.HFovDeg, CesiumPrefetchCameraId, PreloadFov);
	}

	// Tune Cesium3DTileset actors for smoother streaming and LOD quality
	for (TActorIterator<ACesium3DTileset> It(GetWorld()); It; ++It)
	{
		It->MaximumSimultaneousTileLoads = Cfg.MaxSimultaneousTileLoads;
		It->MaximumScreenSpaceError = Cfg.MaximumScreenSpaceError;
		if (Cfg.MaximumCachedBytesMB > 0)
		{
			It->MaximumCachedBytes = static_cast<int64>(Cfg.MaximumCachedBytesMB) * 1024LL * 1024LL;
		}
		It->PreloadAncestors = true;
		It->PreloadSiblings  = false;  // prefetch camera already covers adjacent tiles
		It->ForbidHoles      = false;  // true forces parent tiles to stay rendered while children load,
		                               // growing the render set linearly as camera moves
		It->LoadingDescendantLimit = Cfg.LoadingDescendantLimit;

		// Aggressively cull off-screen tiles: allow low-res placeholders outside the frustum
		// instead of keeping full-detail tiles rendered. Reduces draw calls during camera motion.
		It->EnforceCulledScreenSpaceError = true;
		It->CulledScreenSpaceError = 200.0;
		It->SetUseLodTransitions(Cfg.bUseLodTransitions);
		It->LodTransitionLength    = Cfg.LodTransitionLength;
		It->LogSelectionStats      = Cfg.bLogTileSelectionStats;
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: tuned tileset '%s' (maxLoads=%d SSE=%.1f cacheMB=%d descLimit=%d lodBlend=%d logStats=%d)"),
			*It->GetName(), Cfg.MaxSimultaneousTileLoads,
			Cfg.MaximumScreenSpaceError, Cfg.MaximumCachedBytesMB,
			Cfg.LoadingDescendantLimit, (int)Cfg.bUseLodTransitions,
			(int)Cfg.bLogTileSelectionStats);
	}

	// Apply Cesium backend config (ion server, terrain source, imagery overlay).
	// Must run after the streaming-params loop above.
	UCesiumIonServer* CesiumServer = ApplyCesiumBackendConfig(GetWorld(), Cfg.CesiumBackend);
	Subsystem->StoreCesiumIonServer(CesiumServer);

	// Initialize CPU-side sensor post-processing pipeline (Phase 11)
	// Concrete FSensorPostProcess is stored behind the IPixelPipeline interface
	// so that a NullPixelPipeline or alternate waveband implementation can
	// be substituted without touching the camera actor.
	auto* Pipeline = new FSensorPostProcess();
	Pipeline->Initialize(Cfg.CaptureWidth, Cfg.CaptureHeight, Cfg.SensorModeConfigs, Cfg.ActiveSensorQuality);

	// Wire up CPU-side lens distortion (Phase 15B)
	if (Cfg.OpticalRealism.bEnabled && Cfg.OpticalRealism.bLensDistortion)
	{
		Pipeline->SetDistortion(Cfg.OpticalRealism.DistortionK1, Cfg.OpticalRealism.DistortionK2);
	}

	// Phase 18: weather & atmospheric sensor effects
	Pipeline->SetPhase18Config(Cfg.Phase18);

	Pipeline->SetOverlayConfig(Cfg.OverlayConfig);

	// Phase 21F.1: pass laser designator config to sensor pipeline
	Pipeline->SetLaserDesignatorConfig(Cfg.LaserDesignator);

	SensorFX.Reset(Pipeline);

	// Apply GPU-side optical realism post-process settings (Phase 15)
	if (Cfg.OpticalRealism.bEnabled && SceneCapture)
	{
		FPostProcessSettings& PP = SceneCapture->PostProcessSettings;

		// 15A Motion Blur
		SceneCapture->ShowFlags.SetMotionBlur(Cfg.OpticalRealism.bMotionBlur);
		if (Cfg.OpticalRealism.bMotionBlur)
		{
			PP.bOverride_MotionBlurAmount = true;
			PP.MotionBlurAmount = Cfg.OpticalRealism.MotionBlurAmount;
			PP.bOverride_MotionBlurMax = true;
			PP.MotionBlurMax = static_cast<float>(Cfg.OpticalRealism.MotionBlurMax);
		}

		// 15C Bloom
		SceneCapture->ShowFlags.SetBloom(Cfg.OpticalRealism.bBloom);
		if (Cfg.OpticalRealism.bBloom)
		{
			PP.bOverride_BloomIntensity = true;
			PP.BloomIntensity = Cfg.OpticalRealism.BloomIntensity;
			PP.bOverride_BloomThreshold = true;
			PP.BloomThreshold = Cfg.OpticalRealism.BloomThreshold;
		}

		// 15D Chromatic Aberration
		if (Cfg.OpticalRealism.bChromaticAberration)
		{
			PP.bOverride_SceneFringeIntensity = true;
			PP.SceneFringeIntensity = Cfg.OpticalRealism.ChromaticAberrationIntensity;
		}

		// 15E Depth of Field
		if (Cfg.OpticalRealism.bDepthOfField)
		{
			PP.bOverride_DepthOfFieldFstop = true;
			PP.DepthOfFieldFstop = Cfg.OpticalRealism.ApertureFStop;
			PP.bOverride_DepthOfFieldSensorWidth = true;
			PP.DepthOfFieldSensorWidth = Cfg.OpticalRealism.SensorWidth;
			if (Cfg.OpticalRealism.FocalDistance > 0.0f)
			{
				PP.bOverride_DepthOfFieldFocalDistance = true;
				PP.DepthOfFieldFocalDistance = Cfg.OpticalRealism.FocalDistance;
			}
		}

		// 15F Lens Flare
		SceneCapture->ShowFlags.SetLensFlares(Cfg.OpticalRealism.bLensFlare);
		if (Cfg.OpticalRealism.bLensFlare)
		{
			PP.bOverride_LensFlareIntensity = true;
			PP.LensFlareIntensity = Cfg.OpticalRealism.LensFlareIntensity;
			PP.bOverride_LensFlareBokehSize = true;
			PP.LensFlareBokehSize = Cfg.OpticalRealism.LensFlareBokehSize;
			PP.bOverride_LensFlareThreshold = true;
			PP.LensFlareThreshold = Cfg.OpticalRealism.LensFlareThreshold;
		}

		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: optical realism enabled (blur=%d bloom=%d CA=%d DoF=%d flare=%d distort=%d)"),
			Cfg.OpticalRealism.bMotionBlur, Cfg.OpticalRealism.bBloom,
			Cfg.OpticalRealism.bChromaticAberration, Cfg.OpticalRealism.bDepthOfField,
			Cfg.OpticalRealism.bLensFlare, Cfg.OpticalRealism.bLensDistortion);
	}

	// Phase 24 — Rendering quality
	{
		const FCamSimConfig::FRenderingQualityConfig& RQ = Cfg.RenderingQuality;
		FPostProcessSettings& PP = SceneCapture->PostProcessSettings;

		// 24A contact shadows (ShowFlag — per-light ContactShadowLength is set in editor)
		SceneCapture->ShowFlags.SetContactShadows(RQ.bContactShadows);

		// 24B ambient occlusion (Lumen GTAO)
		if (RQ.AOIntensity > 0.0f)
		{
			PP.bOverride_AmbientOcclusionIntensity = true;
			PP.AmbientOcclusionIntensity           = RQ.AOIntensity;
			PP.bOverride_AmbientOcclusionRadius    = true;
			PP.AmbientOcclusionRadius              = RQ.AORadius;
		}

		// 24E shadow quality + 24D RT reflections via console variables
		auto SetCVar = [](const TCHAR* Name, float Value)
		{
			if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
				CVar->Set(Value, ECVF_SetByCode);
		};
		auto SetCVarI = [](const TCHAR* Name, int32 Value)
		{
			if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
				CVar->Set(Value, ECVF_SetByCode);
		};
		SetCVarI(TEXT("r.RayTracing"),             RQ.bRayTracingEnabled ? 1 : 0);
		SetCVarI(TEXT("r.RayTracing.Reflections"), RQ.bRayTracedReflections ? 1 : 0);  // 24D
		SetCVar (TEXT("r.Shadow.DistanceScale"),                         RQ.ShadowDistanceScale);
		SetCVarI(TEXT("r.Shadow.Virtual.ResolutionLodBiasDirectional"),  RQ.VSMResolutionBias);
		SetCVarI(TEXT("r.Shadow.Virtual.MaxPhysicalPages"),              RQ.VSMMaxPhysicalPages);

		// 24F TSR screen percentage
		if (RQ.TSRScreenPercentage != 100)
		{
			SetCVarI(TEXT("r.ScreenPercentage"), RQ.TSRScreenPercentage);
		}

		// SceneCapture2D AA: override to FXAA (1). TSR (4) causes temporal
		// ghosting on off-screen captures. FXAA is stateless and works with
		// manual-capture mode.
		SetCVarI(TEXT("r.AntiAliasingMethod"), 1);

		UE_LOG(LogCamSim, Log,
			TEXT("ACamSimCamera: RenderingQuality — shadows=%d contactShadow=%d AO=%.2f "
			     "RTRefl=%d shadowDist=%.1f VSMBias=%d TSR%%=%d AA=FXAA"),
			(int)RQ.bEntityShadows, (int)RQ.bContactShadows, RQ.AOIntensity,
			(int)RQ.bRayTracedReflections, RQ.ShadowDistanceScale,
			RQ.VSMResolutionBias, RQ.TSRScreenPercentage);
	}

	// 27F — Configurable render frame rate
	const float TargetRenderFps = FMath::Clamp(Cfg.Performance.RenderFrameRateHz, 1.0f, 120.0f);
	if (!FMath::IsNearlyEqual(TargetRenderFps, 30.0f))
	{
		GEngine->SetMaxFPS(TargetRenderFps);
		GEngine->FixedFrameRate    = TargetRenderFps;
		GEngine->bUseFixedFrameRate = true;
	}

	// 27G — Texture streaming pool budget
	if (Cfg.Performance.TexturePoolBudgetMB > 0)
	{
		const FString TexturePoolCmd = FString::Printf(
			TEXT("r.Streaming.PoolSize %d"), Cfg.Performance.TexturePoolBudgetMB);
		GEngine->Exec(GetWorld(), *TexturePoolCmd);
	}

	UE_LOG(LogCamSim, Log,
		TEXT("ACamSimCamera: Performance — renderFPS=%.0f outputFPS=%.0f texturePoolMB=%d dropTracking=%s hotReload=%s"),
		TargetRenderFps,
		Cfg.Performance.OutputFrameRateHz,
		Cfg.Performance.TexturePoolBudgetMB,
		Cfg.Performance.bTrackFrameDropsByCategory ? TEXT("1") : TEXT("0"),
		Cfg.Performance.bHotReloadConfig ? TEXT("1") : TEXT("0"));

	// 27A — GPU sensor post-process material
	if (Cfg.Performance.bGpuSensorEffects)
	{
		GpuSensorMat_ = Cast<UMaterialInterface>(
			StaticLoadObject(UMaterialInterface::StaticClass(), nullptr,
				*Cfg.Performance.GpuSensorMaterialPath));
		GpuSensorMpc_ = Cast<UMaterialParameterCollection>(
			StaticLoadObject(UMaterialParameterCollection::StaticClass(), nullptr,
				*Cfg.Performance.GpuSensorMpcPath));

		if (GpuSensorMat_ && GpuSensorMpc_ && SceneCapture)
		{
			FWeightedBlendable Blendable;
			Blendable.Object = GpuSensorMat_;
			Blendable.Weight = 1.0f;
			SceneCapture->PostProcessSettings.WeightedBlendables.Array.Add(Blendable);

			// Bypass expensive CPU pipeline loops — defect pixels, quantization, overlay still run
			SensorFX->SetGpuSensorEffectsActive(true);
			UE_LOG(LogCamSim, Log,
				TEXT("ACamSimCamera: GPU sensor pipeline ACTIVE — material=%s"),
				*Cfg.Performance.GpuSensorMaterialPath);
		}
		else
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("ACamSimCamera: GPU sensor material/MPC not found at '%s' / '%s' — falling back to CPU pipeline"),
				*Cfg.Performance.GpuSensorMaterialPath, *Cfg.Performance.GpuSensorMpcPath);
		}
	}

	// Allocate async GPU readback helper (non-blocking DMA: EnqueueCopy → IsReady → Lock)
	GPUReadback = new FRHIGPUTextureReadback(TEXT("CamSimReadback"));

	// Depth capture for ML training data (Phase 17A)
	if (Cfg.MLTraining.bEnabled && Cfg.MLTraining.bDepthMap)
	{
		DepthCapture = NewObject<USceneCaptureComponent2D>(this, TEXT("DepthCapture"));
		DepthCapture->SetupAttachment(Root);
		DepthCapture->bCaptureEveryFrame   = false;
		DepthCapture->bCaptureOnMovement   = false;
		DepthCapture->CaptureSource        = SCS_SceneDepth;
		DepthCapture->bAlwaysPersistRenderingState = false;
		DepthCapture->RegisterComponent();

		DepthRenderTargets.Reset();
		for (int32 Idx = 0; Idx < 2; ++Idx)
		{
			UTextureRenderTarget2D* DRT = NewObject<UTextureRenderTarget2D>(
				this, *FString::Printf(TEXT("CamSimDepthRT_%d"), Idx));
			DRT->InitCustomFormat(Cfg.CaptureWidth, Cfg.CaptureHeight, PF_R32_FLOAT,
				/*bInForceLinearGamma=*/true);
			DRT->UpdateResource();
			DepthRenderTargets.Add(DRT);
		}
		DepthCaptureTargetIndex = 0;
		DepthCapture->TextureTarget = DepthRenderTargets[0];
		DepthGPUReadback = new FRHIGPUTextureReadback(TEXT("CamSimDepthReadback"));
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: depth capture enabled (%dx%d PF_R32_FLOAT)"),
			Cfg.CaptureWidth, Cfg.CaptureHeight);
	}

	// Create persistent encoder thread (decouples sensor post-process from encode)
	{
		IFrameSink* Enc = Subsystem->GetVideoEncoder();
		if (Enc)
		{
			const float OutputFps = (Cfg.Performance.OutputFrameRateHz > 0.0f)
				? Cfg.Performance.OutputFrameRateHz : Cfg.FrameRate;
			EncoderThread = MakeUnique<FEncoderThread>(Enc, OutputFps);
			EncoderThread->Start();
		}
	}

	UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: ready (%dx%d @ %.0ffps)"),
		Cfg.CaptureWidth, Cfg.CaptureHeight, Cfg.FrameRate);
}

// -------------------------------------------------------------------------
// EndPlay
// -------------------------------------------------------------------------

void ACamSimCamera::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	{
		ACesiumCameraManager* CamMgr = ACesiumCameraManager::GetDefaultCameraManager(this);
		if (CamMgr)
		{
			if (CesiumCameraId >= 0)
			{
				CamMgr->RemoveCamera(CesiumCameraId);
				CesiumCameraId = -1;
			}
			if (CesiumPrefetchCameraId >= 0)
			{
				CamMgr->RemoveCamera(CesiumPrefetchCameraId);
				CesiumPrefetchCameraId = -1;
			}
		}
	}

	// Stop encoder thread before encoder Close (subsystem owns the encoder)
	if (EncoderThread)
	{
		EncoderThread->Stop();
		EncoderThread.Reset();
	}

	// Phase 27B — deregister from subsystem so health writer doesn't access a dangling ptr
	if (Subsystem)
	{
		Subsystem->RegisterCamera(nullptr);
	}

	// Free the GPU readback object — must happen before the render target is destroyed
	if (GPUReadback)
	{
		delete GPUReadback;
		GPUReadback = nullptr;
	}

	if (DepthGPUReadback)
	{
		delete DepthGPUReadback;
		DepthGPUReadback = nullptr;
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
	static double LastHeartbeatWallSec = 0.0;
	if (++TickCount % 150 == 0)
	{
		const double NowSec = FPlatformTime::Seconds();
		const double WallDeltaSec = (LastHeartbeatWallSec > 0.0)
			? (NowSec - LastHeartbeatWallSec)
			: 0.0;
		const double EffectiveFps = (WallDeltaSec > 0.0) ? (150.0 / WallDeltaSec) : 0.0;
		LastHeartbeatWallSec = NowSec;

		const int32 ReadyPollsRequired = FMath::Max(1, Subsystem->GetConfig().ReadbackReadyPolls);
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: tick=%llu busy=%d readback=%d sensor=%d frames_encoded=%llu dropped=%llu cap_idx=%d pending_idx=%d ready_polls=%d wall_dt=%.1fs fps=%.1f"),
			TickCount, (int)(bool)bSensorBusy, (int)(bool)bReadbackPending,
			(int)(SensorComp && SensorComp->IsOn()), FrameIndex, (uint64)DroppedFrameCount,
			CaptureTargetIndex, PendingReadbackTargetIndex, ReadyPollsRequired,
			WallDeltaSec, EffectiveFps);

		// Tile loading stats — report per-tileset load progress and memory
		for (TActorIterator<ACesium3DTileset> It(GetWorld()); It; ++It)
		{
			const float Progress = It->GetLoadProgress();
			int32 TilesLoaded = 0;
			int64 DataBytes = 0;
			if (auto* Tileset = It->GetTileset())
			{
				TilesLoaded = Tileset->getNumberOfTilesLoaded();
				DataBytes = Tileset->getTotalDataBytes();
			}
			UE_LOG(LogCamSim, Log,
				TEXT("ACamSimCamera: tileset='%s' progress=%.1f%% SSE=%.1f loaded=%d dataMB=%.1f maxLoads=%d"),
				*It->GetName(), Progress * 100.0f,
				It->MaximumScreenSpaceError,
				TilesLoaded, DataBytes / (1024.0 * 1024.0),
				It->MaximumSimultaneousTileLoads);
		}
	}

	// 27D — Hot-reload config via mtime poll
	{
		const FCamSimConfig& Cfg = Subsystem->GetConfig();
		if (Cfg.Performance.bHotReloadConfig)
		{
			HotReloadAccumSec_ += DeltaTime;
			if (HotReloadAccumSec_ >= Cfg.Performance.HotReloadPollIntervalSec)
			{
				HotReloadAccumSec_ = 0.0f;
				const FString CfgPath = FCamSimConfig::GetConfigFilePath();
				const FDateTime CurrMTime = IFileManager::Get().GetTimeStamp(*CfgPath);
				if (CurrMTime != FDateTime::MinValue() && CurrMTime != LastConfigMTime_)
				{
					LastConfigMTime_ = CurrMTime;
					const FCamSimConfig OldCfg = Cfg;
					FCamSimConfig NewCfg = FCamSimConfig::Load();
					if (!NewCfg.bLoadedSuccessfully)
					{
						UE_LOG(LogCamSim, Warning, TEXT("HotReload: config parse failed — keeping current config"));
						return;
					}

					// Warn on immutable field changes (require restart)
					if (NewCfg.CigiPort != OldCfg.CigiPort)
						UE_LOG(LogCamSim, Warning, TEXT("HotReload: CIGI port change ignored (requires restart)"));
					if (NewCfg.MulticastAddr != OldCfg.MulticastAddr)
						UE_LOG(LogCamSim, Warning, TEXT("HotReload: multicast addr change ignored (requires restart)"));
					if (NewCfg.VideoCodec != OldCfg.VideoCodec)
						UE_LOG(LogCamSim, Warning, TEXT("HotReload: video codec change ignored (requires restart)"));
					if (NewCfg.MulticastPort != OldCfg.MulticastPort)
						UE_LOG(LogCamSim, Warning, TEXT("HotReload: multicast port change ignored (requires restart)"));

					// Apply mutable settings to subsystem config
					Subsystem->HotReloadConfig(NewCfg);

					// Re-apply mutable pixel pipeline settings
					SensorFX->SetPhase18Config(NewCfg.Phase18);
					SensorFX->SetOverlayConfig(NewCfg.OverlayConfig);
					SensorFX->SetLaserDesignatorConfig(NewCfg.LaserDesignator);

					UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: HotReload applied from %s"), *CfgPath);
				}
			}
		}
	}

	// Apply pending CIGI state first (CIGI queue drain runs every tick,
	// even if we skip capture, so the platform pose stays current)
	ApplyCigiState(DeltaTime);

	// 27A — Push current sensor state into GPU MPC each tick (no-op when MPC not loaded)
	if (GpuSensorMpc_) UpdateGpuSensorMpcParams();

	// ── PHASE 1: Complete pending readback ──────────────────────────────
	// RHI Lock()/Unlock() must run on the render thread — we enqueue a render
	// command that does IsReady/Lock/Copy/Unlock, then FlushRenderingCommands()
	// to synchronously wait.  The flush is cheap (~0.1ms on Metal) because the
	// only pending work is the tiny poll command; it replaces the old async
	// approach that wasted an entire tick waiting for the render-thread round-trip.
	if (bReadbackPending && bReadbackDMAIssued.Load(EMemoryOrder::Relaxed))
	{
		const FCamSimConfig& Cfg = Subsystem->GetConfig();
		const int32 ReadyPollsRequired = FMath::Max(1, Cfg.ReadbackReadyPolls);
		FRHIGPUTextureReadback* Readback = GPUReadback;
		FRHIGPUTextureReadback* DepthReadback = DepthGPUReadback;
		UTextureRenderTarget2D* RT =
			RenderTargets.IsValidIndex(PendingReadbackTargetIndex)
				? RenderTargets[PendingReadbackTargetIndex].Get() : nullptr;

		// Stack-local results filled by render command; safe because
		// FlushRenderingCommands() blocks game thread until it completes.
		TArray<FColor> Pixels;
		TArray<float>  DepthPixels;
		bool bGotPixels = false;
		bool bReadbackFailed = false;

		ENQUEUE_RENDER_COMMAND(CamSimPollReadback)(
			[&, Readback, DepthReadback, RT, ReadyPollsRequired](FRHICommandListImmediate&)
		{
			if (!Readback || !Readback->IsReady())
			{
				ReadbackReadyStreakGT = 0;
				return;
			}
			if (ReadbackReadyStreakGT < 255) ++ReadbackReadyStreakGT;
			if (ReadbackReadyStreakGT < ReadyPollsRequired) return;

			int32 RowPitch = 0;
			void* RawData = Readback->Lock(RowPitch);
			if (!RawData || !RT)
			{
				if (RawData) Readback->Unlock();
				bReadbackFailed = true;
				return;
			}

			const int32 W = RT->SizeX;
			const int32 H = RT->SizeY;
			const EPixelFormat PixelFormat = RT->GetFormat();
			const bool bIsBgra = (PixelFormat == PF_B8G8R8A8);
			const bool bIsRgba = (PixelFormat == PF_R8G8B8A8);
			if (!bIsBgra && !bIsRgba)
			{
				Readback->Unlock();
				bReadbackFailed = true;
				return;
			}

			Pixels.SetNumUninitialized(W * H);
			CamSimConvertReadbackPixels(RawData, RowPitch, W, H,
				PixelFormat, Cfg.ReadbackFormat, Cfg.bSwapRBReadback,
				Pixels, PendingFrameIndex);
			Readback->Unlock();

			// Opportunistic depth readback
			if (DepthReadback && DepthReadback->IsReady())
			{
				if (DepthReadbackReadyStreakGT < 255) ++DepthReadbackReadyStreakGT;
				if (DepthReadbackReadyStreakGT >= ReadyPollsRequired)
				{
					int32 DepthRowPitch = 0;
					void* DepthRaw = DepthReadback->Lock(DepthRowPitch);
					if (DepthRaw)
					{
						const int32 CaptureW = Cfg.CaptureWidth;
						const int32 CaptureH = Cfg.CaptureHeight;
						DepthPixels.SetNumUninitialized(CaptureW * CaptureH);
						const uint8* Src = static_cast<const uint8*>(DepthRaw);
						float*       Dst = DepthPixels.GetData();
						const int32  RowBytes = CaptureW * sizeof(float);
						for (int32 Row = 0; Row < CaptureH; ++Row)
						{
							FMemory::Memcpy(Dst + Row * CaptureW, Src + Row * DepthRowPitch, RowBytes);
						}
						for (float& V : DepthPixels) { V /= 100.0f; }
						DepthReadback->Unlock();
					}
					// else: Lock() returned null — do NOT call Unlock() (UB)
				}
			}
			else
			{
				DepthReadbackReadyStreakGT = 0;
			}

			bGotPixels = true;
		});

		// Synchronously wait for the render command — game thread was idle during
		// the old async poll anyway, so this adds no effective latency.
		FlushRenderingCommands();

		if (bGotPixels)
		{
			bReadbackPending = false;
			PendingReadbackTargetIndex = INDEX_NONE;

			if (!bSensorBusy)
			{
				bSensorBusy = true;
				SubmitFrameToEncoder(MoveTemp(Pixels), PendingTelemetry,
					PendingFrameIndex, MoveTemp(DepthPixels));
			}
			else
			{
				// Queue for dispatch when sensor frees (Phase 2)
				CompletedPixels_      = MoveTemp(Pixels);
				CompletedDepth_       = MoveTemp(DepthPixels);
				CompletedTelemetry_   = PendingTelemetry;
				CompletedFrameIndex_  = PendingFrameIndex;
				bReadbackResultReady_ = true;
			}
		}
		else if (bReadbackFailed)
		{
			if (bTrackFrameDrops_) FrameDropStats_.ReadbackTimeout++;
			bReadbackPending = false;
			PendingReadbackTargetIndex = INDEX_NONE;
			UE_LOG(LogCamSim, Warning,
				TEXT("CamSimReadback frame %llu: lock returned null or bad format"), PendingFrameIndex);
		}
	}

	// ── PHASE 2: Dispatch queued result when sensor becomes free ────────
	if (bReadbackResultReady_ && !bSensorBusy)
	{
		bSensorBusy = true;
		SubmitFrameToEncoder(MoveTemp(CompletedPixels_), CompletedTelemetry_,
			CompletedFrameIndex_, MoveTemp(CompletedDepth_));
		bReadbackResultReady_ = false;
	}

	// ── PHASE 3: Sensor off check ──────────────────────────────────────
	if (SensorComp && !SensorComp->IsOn()) return;

	// ── PHASE 4: Output decimation ─────────────────────────────────────
	{
		const FCamSimConfig& DecCfg = Subsystem->GetConfig();
		const float RenderFps = DecCfg.Performance.RenderFrameRateHz;
		const float OutputFps = DecCfg.Performance.OutputFrameRateHz;
		if (RenderFps > OutputFps && OutputFps > 0.0f)
		{
			RenderFrameCounter_++;
			const uint64 DecimationRatio = FMath::RoundToInt64(RenderFps / OutputFps);
			if (DecimationRatio > 1 && (RenderFrameCounter_ % DecimationRatio) != 0)
			{
				return;  // skip capture on this render frame
			}
		}
	}

	// ── PHASE 5: Issue new capture if readback slot is free ─────────────
	if (!bReadbackPending)
	{
		CaptureAndEncode();
	}
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
			// Cesium's ENU transform can emit a NaN world scale during origin rebasing;
			// unconditionally reset to (1,1,1) to prevent the UE SetRelativeScale3D warning.
			SetActorScale3D(FVector::OneVector);
			SetActorRotation(FRotator(EntityState.Pitch, EntityState.Yaw, EntityState.Roll));

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

			// Phase 26C: compute ground speed from position delta (Tag 8)
			if (bHasPrevGeoPos_ && DeltaTime > 0.0f)
			{
				// Haversine approximation using Unreal's cosine
				constexpr double EarthR = 6371000.0; // metres
				const double DLat = FMath::DegreesToRadians(EntityState.Latitude  - PrevGeoLatDeg_);
				const double DLon = FMath::DegreesToRadians(EntityState.Longitude - PrevGeoLonDeg_);
				const double MeanLat = FMath::DegreesToRadians(
					(EntityState.Latitude + PrevGeoLatDeg_) * 0.5);
				const double Dx = DLon * FMath::Cos(MeanLat) * EarthR;
				const double Dy = DLat * EarthR;
				const double DistM = FMath::Sqrt(Dx * Dx + Dy * Dy);
				CurrentTelemetry.GroundSpeedMps = static_cast<float>(DistM / DeltaTime);
			}
			PrevGeoLatDeg_  = EntityState.Latitude;
			PrevGeoLonDeg_  = EntityState.Longitude;
			bHasPrevGeoPos_ = true;

		}
		else if (!bGotState)
		{
			// No CIGI update this frame — decay ground speed to zero
			CurrentTelemetry.GroundSpeedMps = 0.0f;
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
		CurrentTelemetry.SensorMode = static_cast<uint8>(SensorComp->GetMode());
		CurrentTelemetry.SensorPolarity = SensorComp->GetPolarity();
	}

	// -----------------------------------------------------------------------
	// View Control (opcode 16) + Camera ArtPart → delegated to UCamSimGimbalComponent
	// -----------------------------------------------------------------------
	if (GimbalComp)
	{
		GimbalComp->TickGimbal(DeltaTime, Receiver, Cfg);

		// Phase 22G: Detect FPS activation/deactivation from CIGI ViewControl
		{
			const uint16 CigiReqId = GimbalComp->GetLastViewControlEntityId();
			if (CigiReqId != 0 && CigiReqId != FpsEntityId_)
			{
				FpsEntityId_ = CigiReqId;
				bFpsFromCigi_ = true;
				UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: FPS mode -> entity %u (CIGI ViewControl)"), FpsEntityId_);
			}
			else if (CigiReqId == 0 && bFpsFromCigi_ && FpsEntityId_ != 0)
			{
				UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: FPS mode deactivated (CIGI ViewControl EntityId=0)"));
				FpsEntityId_ = 0;
				bFpsFromCigi_ = false;
			}
		}

		// Phase 22G: Apply FPS pose if active (overrides CIGI camera entity position)
		if (FpsEntityId_ != 0)
		{
			ApplyFpsPose();
		}

		// Apply result to the scene capture's relative rotation
		if (SceneCapture)
		{
			SceneCapture->SetRelativeRotation(GimbalComp->GetGimbalRelativeRotation());
		}

		// Sync into telemetry for KLV output
		CurrentTelemetry.GimbalYaw   = GimbalComp->GetGimbalYaw();
		CurrentTelemetry.GimbalPitch = GimbalComp->GetGimbalPitch();
		CurrentTelemetry.GimbalRoll  = GimbalComp->GetGimbalRoll();

		// 27E — Dynamic tile prefetch during fast gimbal slews
		if (Cfg.Performance.TilePrefetchSlewThresholdDegPerSec > 0.0f)
		{
			if (DeltaTime > KINDA_SMALL_NUMBER)
			{
				const float CurrPan  = CurrentTelemetry.GimbalYaw;
				const float CurrTilt = CurrentTelemetry.GimbalPitch;
				const float PanVelDegSec  = FMath::Abs(CurrPan  - PrevGimbalPanDeg_)  / DeltaTime;
				const float TiltVelDegSec = FMath::Abs(CurrTilt - PrevGimbalTiltDeg_) / DeltaTime;
				const float MaxVel = FMath::Max(PanVelDegSec, TiltVelDegSec);

				if (MaxVel >= Cfg.Performance.TilePrefetchSlewThresholdDegPerSec)
				{
					// Reset (or extend) the boost window on every above-threshold frame
					TilePrefetchBoostFramesRemaining_ = Cfg.Performance.TilePrefetchBoostFrames;
				}
				else if (TilePrefetchBoostFramesRemaining_ > 0)
				{
					TilePrefetchBoostFramesRemaining_--;
				}

				// Apply or restore Cesium SSE based on current counter
				for (TActorIterator<ACesium3DTileset> It(GetWorld()); It; ++It)
				{
					It->MaximumScreenSpaceError = (TilePrefetchBoostFramesRemaining_ > 0)
						? Cfg.MaximumScreenSpaceError / FMath::Max(Cfg.Performance.TilePrefetchFovBoost, 1.0f)
						: Cfg.MaximumScreenSpaceError;
				}
			}

			// Always update prev angles regardless of DeltaTime
			PrevGimbalPanDeg_  = CurrentTelemetry.GimbalYaw;
			PrevGimbalTiltDeg_ = CurrentTelemetry.GimbalPitch;
		}
	}

	// Adaptive SSE — dynamically adjust Cesium LOD based on frame budget
	if (Cfg.Performance.bAdaptiveSSE && DeltaTime > KINDA_SMALL_NUMBER
		&& TilePrefetchBoostFramesRemaining_ <= 0)  // don't fight prefetch boost
	{
		const float BudgetSec = 1.0f / FMath::Max(Cfg.Performance.OutputFrameRateHz, 1.0f);
		if (CurrentAdaptiveSSE_ <= 0.0f)
			CurrentAdaptiveSSE_ = Cfg.MaximumScreenSpaceError;  // initialise on first frame

		if (DeltaTime > BudgetSec)
		{
			// Over budget — increase SSE (coarser LOD) by 1.0
			CurrentAdaptiveSSE_ = FMath::Min(CurrentAdaptiveSSE_ + 1.0f, Cfg.Performance.AdaptiveSSEMax);
			UnderBudgetStreakFrames_ = 0;
		}
		else if (DeltaTime < BudgetSec * 0.75f)
		{
			// Under 75% budget for 30 consecutive frames — decrease SSE (sharper)
			UnderBudgetStreakFrames_++;
			if (UnderBudgetStreakFrames_ >= 30)
			{
				CurrentAdaptiveSSE_ = FMath::Max(CurrentAdaptiveSSE_ - 0.5f, Cfg.Performance.AdaptiveSSEMin);
				UnderBudgetStreakFrames_ = 0;
			}
		}
		else
		{
			UnderBudgetStreakFrames_ = 0;
		}

		// Apply adapted SSE to all tilesets
		for (TActorIterator<ACesium3DTileset> It(GetWorld()); It; ++It)
		{
			It->MaximumScreenSpaceError = CurrentAdaptiveSSE_;
		}
	}

	// Populate environment telemetry for sensor effects (Phase 16K + Phase 18)
	ReadEnvironmentTelemetry();

	// Update Cesium tile streaming camera *after* all state (position, gimbal,
	// FOV) has been applied this frame so LOD decisions use the true frustum.
	UpdateCesiumCamera();

	// Compute slant range and frame centre from current pose + gimbal
	ComputeGeometricLOS();

	// Phase 21F.2 — dynamic laser designator from DIS Designator PDU
	if (Subsystem)
	{
		FDisEntityAdapter* DisAdapter = Subsystem->GetDisAdapter();
		if (DisAdapter)
		{
			double DesigLat, DesigLon, DesigAlt;
			int32 DesigCode;
			if (DisAdapter->GetDesignatorSpot(DesigLat, DesigLon, DesigAlt, DesigCode))
			{
				// Project geodetic → screen coordinates
				const FCamSimConfig& DesigCfg = Subsystem->GetConfig();
				FCamSimConfig::FLaserDesignatorConfig LaserCfg = DesigCfg.LaserDesignator;
				LaserCfg.bEnabled = true;
				LaserCfg.DesignatorCode = DesigCode;

				// Build view projection matrix from current camera pose
				const FVector CamLoc = GetActorLocation();
				const FRotator CamRot = GetActorRotation();
				const float HFOV = SceneCapture ? SceneCapture->FOVAngle : DesigCfg.HFovDeg;

				FMatrix ViewProj = FEntityProjection::BuildViewProjectionMatrix(
					CamLoc, CamRot, HFOV, DesigCfg.CaptureWidth, DesigCfg.CaptureHeight);

				// Convert designator geodetic → UE world position via geospatial provider
				const FCamSimGeospatialProvider* GeoProvider = Subsystem->GetGeospatialProvider();
				FVector SpotUE;
				if (GeoProvider && GeoProvider->GeoToWorld(GetWorld(), DesigLat, DesigLon, DesigAlt, SpotUE))
				{
					// Project to screen
					FVector4 Clip = ViewProj.TransformFVector4(FVector4(SpotUE, 1.0f));
					if (Clip.W > 0.0f)
					{
						const float NdcX = Clip.X / Clip.W;
						const float NdcY = Clip.Y / Clip.W;
						LaserCfg.SpotX = FMath::Clamp((NdcX + 1.0f) * 0.5f, 0.0f, 1.0f);
						LaserCfg.SpotY = FMath::Clamp((1.0f - NdcY) * 0.5f, 0.0f, 1.0f);
					}
				}

				if (SensorFX)
				{
					SensorFX->SetLaserDesignatorConfig(LaserCfg);
				}
			}
		}
	}

	// Auto-focus DoF from slant range (Phase 15E)
	if (Subsystem && SceneCapture)
	{
		const FCamSimConfig& OptCfg = Subsystem->GetConfig();
		if (OptCfg.OpticalRealism.bEnabled && OptCfg.OpticalRealism.bDepthOfField
			&& OptCfg.OpticalRealism.FocalDistance <= 0.0f
			&& CurrentTelemetry.SlantRangeM > 0.0)
		{
			SceneCapture->PostProcessSettings.bOverride_DepthOfFieldFocalDistance = true;
			SceneCapture->PostProcessSettings.DepthOfFieldFocalDistance =
				static_cast<float>(CurrentTelemetry.SlantRangeM * 100.0); // m → cm
		}
	}
}

// -------------------------------------------------------------------------
// UpdateGpuSensorMpcParams – Phase 27A: push per-frame scalars to GPU MPC
// -------------------------------------------------------------------------

void ACamSimCamera::UpdateGpuSensorMpcParams()
{
	if (!GpuSensorMpc_ || !GetWorld()) return;
	const FCamSimConfig& Cfg = Subsystem->GetConfig();
	const FSensorModeConfig* IrCfg = Cfg.SensorModeConfigs.Find(ESensorMode::IR);
	const float NoiseIntensity = IrCfg ? IrCfg->NETD : 0.0f;
	UKismetMaterialLibrary::SetScalarParameterValue(GetWorld(), GpuSensorMpc_,
		TEXT("SensorMode"), static_cast<float>(SensorComp->GetMode()));
	UKismetMaterialLibrary::SetScalarParameterValue(GetWorld(), GpuSensorMpc_,
		TEXT("NoiseIntensity"), NoiseIntensity);
}

// -------------------------------------------------------------------------
// ReadEnvironmentTelemetry – sun elevation + Phase 18 atmospheric snapshot
// -------------------------------------------------------------------------

void ACamSimCamera::ReadEnvironmentTelemetry()
{
	if (UWorld* W = GetWorld())
	{
		for (TActorIterator<ACamSimEnvironment> It(W); It; ++It)
		{
			CurrentTelemetry.SunElevationDeg = It->GetSunElevationDeg();

			// Phase 18: atmospheric snapshot
			const ACamSimEnvironment::FAtmosphericSnapshot Snap = It->GetAtmosphericSnapshot();
			CurrentTelemetry.AtmosphericVisibilityM = Snap.AtmosphericVisibilityM;
			CurrentTelemetry.RelativeHumidity       = Snap.RelativeHumidity;
			CurrentTelemetry.AirTempCelsius         = Snap.AirTempCelsius;
			CurrentTelemetry.WeatherSeverity        = Snap.WeatherSeverity;
			CurrentTelemetry.WeatherPrecipType      = Snap.WeatherPrecipType;

			// 18L: update environment with camera position for zone blending
			It->SetCameraPosition(CurrentTelemetry.Latitude, CurrentTelemetry.Longitude);
			break;
		}
	}
}

// -------------------------------------------------------------------------
// ComputeGeometricLOS – slant range and frame center from platform + gimbal
// -------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Phase 22G: First-person view — attach camera to tracked entity
// ---------------------------------------------------------------------------

void ACamSimCamera::ApplyFpsPose()
{
	if (!Subsystem || !GlobeAnchor) return;

	FCamSimEntityManager* Mgr = Subsystem->GetEntityManager();
	if (!Mgr) return;

	ACamSimEntity* Entity = Mgr->FindEntity(FpsEntityId_);
	if (!IsValid(Entity)) return;  // entity not yet spawned — hold current position

	UCesiumGlobeAnchorComponent* EntGa =
		Entity->FindComponentByClass<UCesiumGlobeAnchorComponent>();
	if (!EntGa) return;

	// Read entity geodetic position (Cesium LLH order: Lon, Lat, Height)
	const FVector LLH = EntGa->GetLongitudeLatitudeHeight();
	const double EntityLon = LLH.X;
	const double EntityLat = LLH.Y;
	const double EntityAlt = LLH.Z;

	// Eye height: simple vertical offset above entity origin
	const double EyeAlt = EntityAlt + static_cast<double>(FpsEyeHeightM_);

	GlobeAnchor->MoveToLongitudeLatitudeHeight(
		FVector(EntityLon, EntityLat, EyeAlt));
	SetActorScale3D(FVector::OneVector);

	// Inherit entity heading; gimbal applies freelook on top
	const FRotator EntityRot = Entity->GetActorRotation();
	SetActorRotation(FRotator(0.0f, EntityRot.Yaw, 0.0f));

	// Update telemetry from entity pose
	CurrentTelemetry.Latitude  = EntityLat;
	CurrentTelemetry.Longitude = EntityLon;
	CurrentTelemetry.Altitude  = static_cast<float>(EyeAlt);
	CurrentTelemetry.Yaw       = EntityRot.Yaw;
	CurrentTelemetry.Pitch     = 0.0f;
	CurrentTelemetry.Roll      = 0.0f;
}

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

			// Convert UE world-space hit point to geodetic via configured geospatial provider
			const FCamSimGeospatialProvider* GeoProvider = Subsystem ? Subsystem->GetGeospatialProvider() : nullptr;
			if (GeoProvider)
			{
				double HitLat = 0.0;
				double HitLon = 0.0;
				double HitAlt = 0.0;
				if (GeoProvider->WorldToGeo(World, Hit.Location, HitLat, HitLon, HitAlt))
				{
					CurrentTelemetry.FrameCenterLon  = HitLon;
					CurrentTelemetry.FrameCenterLat  = HitLat;
					CurrentTelemetry.FrameCenterElev = HitAlt;
				}
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
	if (!Subsystem) return;

	ACesiumCameraManager* CamMgr = ACesiumCameraManager::GetDefaultCameraManager(this);
	if (!CamMgr) return;

	const FCamSimConfig& Cfg = Subsystem->GetConfig();
	// Use the live SceneCapture FOV (updated by ViewDef / sensor presets)
	// instead of the static config default, so Cesium streams tiles at the
	// correct resolution for the current zoom level.
	const float LiveHFov = SceneCapture ? SceneCapture->FOVAngle : Cfg.HFovDeg;

	// Primary camera — actual render FOV for accurate tile LOD/SSE
	if (CesiumCameraId >= 0)
	{
		FCesiumCamera PrimaryCam(
			FVector2D(Cfg.CaptureWidth, Cfg.CaptureHeight),
			SceneCapture->GetComponentLocation(),
			SceneCapture->GetComponentRotation(),
			LiveHFov);
		CamMgr->UpdateCamera(CesiumCameraId, PrimaryCam);
	}

	// Prefetch camera — inflated FOV for anticipatory tile loading
	if (CesiumPrefetchCameraId >= 0)
	{
		const float PreloadFov = FMath::Clamp(LiveHFov * Cfg.TilePreloadFovScale, LiveHFov, 179.0f);
		FCesiumCamera PrefetchCam(
			FVector2D(Cfg.CaptureWidth, Cfg.CaptureHeight),
			SceneCapture->GetComponentLocation(),
			SceneCapture->GetComponentRotation(),
			PreloadFov);
		CamMgr->UpdateCamera(CesiumPrefetchCameraId, PrefetchCam);
	}
}

// -------------------------------------------------------------------------
// CaptureAndEncode – State A: trigger GPU capture + enqueue async readback
// -------------------------------------------------------------------------
// State B (polling completion) runs in Tick() via the bReadbackPending branch.
// The two states naturally double-buffer: frame N+1 renders on the game thread
// while the render thread completes frame N's DMA and the encode task runs.

void ACamSimCamera::CaptureAndEncode()
{
	if (!SceneCapture || !GPUReadback) return;
	if (!RenderTargets.IsValidIndex(CaptureTargetIndex) || !RenderTargets[CaptureTargetIndex]) return;

	// Capture entity annotation snapshot BEFORE setting bSensorBusy (Phase 17D)
	if (Subsystem)
	{
		FGroundTruthCollector* Collector  = Subsystem->GetGroundTruthCollector();
		FCamSimEntityManager*  EntityMgr  = Subsystem->GetEntityManager();
		if (Collector && Collector->IsEnabled() && EntityMgr && SceneCapture)
		{
			const FCamSimConfig& Cfg = Subsystem->GetConfig();
			FViewProjectionData ViewProj;
			ViewProj.ViewProjectionMatrix = FEntityProjection::BuildViewProjectionMatrix(
				SceneCapture->GetComponentLocation(),
				SceneCapture->GetComponentRotation(),
				SceneCapture->FOVAngle,
				Cfg.CaptureWidth, Cfg.CaptureHeight);
			ViewProj.ImageWidth  = Cfg.CaptureWidth;
			ViewProj.ImageHeight = Cfg.CaptureHeight;
			Collector->SetPendingEntitySnapshot(
				EntityMgr->GetEntitySnapshot(ViewProj),
				Cfg.CaptureWidth, Cfg.CaptureHeight);
		}
	}

	UTextureRenderTarget2D* CaptureRT = RenderTargets[CaptureTargetIndex].Get();
	SceneCapture->TextureTarget = CaptureRT;

	// Snapshot telemetry immediately before capture so KLV timestamp is accurate
	CurrentTelemetry.TimestampUs =
		static_cast<uint64>(FPlatformTime::Seconds() * 1'000'000.0);

	SceneCapture->CaptureScene();
	PendingReadbackTargetIndex = CaptureTargetIndex;
	CaptureTargetIndex = (CaptureTargetIndex + 1) % RenderTargets.Num();

	PendingFrameIndex = FrameIndex++;
	PendingTelemetry  = CurrentTelemetry;

	// Reset game-thread readback state for the new capture
	bReadbackDMAIssued.Store(false, EMemoryOrder::Relaxed);
	ReadbackReadyStreakGT = 0;
	DepthReadbackReadyStreakGT = 0;
	bReadbackPending = true;

	// Trigger depth capture alongside color (Phase 17A)
	UTextureRenderTarget2D* DepthCaptureRT = nullptr;
	if (DepthCapture && DepthGPUReadback && DepthRenderTargets.IsValidIndex(DepthCaptureTargetIndex))
	{
		DepthCaptureRT = DepthRenderTargets[DepthCaptureTargetIndex].Get();
		DepthCapture->TextureTarget = DepthCaptureRT;
		// Sync relative rotation with SceneCapture so depth aligns with color
		DepthCapture->SetRelativeRotation(SceneCapture->GetRelativeRotation());
		DepthCapture->CaptureScene();
		DepthCaptureTargetIndex = (DepthCaptureTargetIndex + 1) % DepthRenderTargets.Num();
	}

	// Enqueue the async GPU→CPU DMA on the render thread (returns immediately)
	UTextureRenderTarget2D*  RT       =
		RenderTargets.IsValidIndex(PendingReadbackTargetIndex) ? RenderTargets[PendingReadbackTargetIndex].Get() : nullptr;
	FRHIGPUTextureReadback*  Readback = GPUReadback;
	if (!RT)
	{
		bReadbackPending = false;
		PendingReadbackTargetIndex = INDEX_NONE;
		return;
	}

	FRHIGPUTextureReadback* DepthReadback = DepthGPUReadback;

	ENQUEUE_RENDER_COMMAND(CamSimEnqueueReadback)(
		[this, RT, Readback, DepthCaptureRT, DepthReadback](FRHICommandListImmediate& RHICmdList)
	{
		FTextureRenderTargetResource* Resource = RT->GetRenderTargetResource();
		if (!Resource) return;

		FRHITexture* SourceTexture = Resource->GetRenderTargetTexture();

		// Explicit transition: ensure all render target writes from CaptureScene()
		// are visible before the copy.  Metal handles this implicitly via resource
		// tracking, but Vulkan requires an explicit pipeline barrier.
		RHICmdList.Transition(FRHITransitionInfo(
			SourceTexture,
			ERHIAccess::RTV,       // prior: render target view (color attachment write)
			ERHIAccess::CopySrc)); // next:  copy source (transfer read)

		// Non-blocking: kicks off DMA; IsReady() polled on game thread after this flag
		Readback->EnqueueCopy(RHICmdList, SourceTexture);

		// Transition back to render target so the next CaptureScene can write to it
		RHICmdList.Transition(FRHITransitionInfo(
			SourceTexture,
			ERHIAccess::CopySrc,
			ERHIAccess::RTV));

		// Depth readback — enqueue alongside color (Phase 17A)
		if (DepthCaptureRT && DepthReadback)
		{
			FTextureRenderTargetResource* DepthRes = DepthCaptureRT->GetRenderTargetResource();
			FRHITexture* DepthTex = DepthRes ? DepthRes->GetRenderTargetTexture() : nullptr;
			if (DepthTex)
			{
				RHICmdList.Transition(FRHITransitionInfo(DepthTex, ERHIAccess::RTV, ERHIAccess::CopySrc));
				DepthReadback->EnqueueCopy(RHICmdList, DepthTex);
				RHICmdList.Transition(FRHITransitionInfo(DepthTex, ERHIAccess::CopySrc, ERHIAccess::RTV));
			}
		}

		// Signal game thread: DMA has been enqueued, safe to poll IsReady()
		bReadbackDMAIssued.Store(true, EMemoryOrder::Relaxed);
	});
}

// -------------------------------------------------------------------------
// SubmitFrameToEncoder – called from game thread, dispatches async task
// -------------------------------------------------------------------------

void ACamSimCamera::SubmitFrameToEncoder(
	TArray<FColor> PixelData, FCamSimTelemetry Telemetry, uint64 FrameIdx, TArray<float> DepthMetres)
{
	if (!EncoderThread)
	{
		bSensorBusy = false;
		return;
	}

	// Read sensor state from Telemetry — already captured on the game thread in Tick().
	// Do NOT touch SensorComp inside the async task lambda — UObjects are game-thread only.
	const ESensorMode  Mode     = static_cast<ESensorMode>(Telemetry.SensorMode);
	const uint8        Polarity = Telemetry.SensorPolarity;

	// When GPU sensor effects are active (Phase 27A), FSensorPostProcess::Process()
	// bypasses the expensive CPU loops internally (waveband, AGC, noise) and only
	// runs cheap stateful effects (defect pixels, quantization, overlay).
	IPixelPipeline* FX = SensorFX.Get();

	// Ground truth collector (Phase 17) — pointer captured for task lambda.
	FGroundTruthCollector* Collector = Subsystem ? Subsystem->GetGroundTruthCollector() : nullptr;
	const int32 CaptureW = Subsystem ? Subsystem->GetConfig().CaptureWidth  : 0;
	const int32 CaptureH = Subsystem ? Subsystem->GetConfig().CaptureHeight : 0;

	// Capture raw pointer to encoder thread for lambda (safe: thread outlives task)
	FEncoderThread* EncThread = EncoderThread.Get();

	// Sensor post-process runs on a pool thread; encode is decoupled via SPSC queue.
	// bSensorBusy is cleared after sensor FX + ML annotation, NOT after encode.
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
		[this, EncThread, FX, Mode, Polarity, Collector, CaptureW, CaptureH,
		 Pixels = MoveTemp(PixelData), Telemetry, FrameIdx,
		 Depth  = MoveTemp(DepthMetres)]() mutable
	{
		SCOPE_CYCLE_COUNTER(STAT_CamSimEncode);
		// Apply CPU-side sensor post-processing before encode
		if (FX)
		{
			FX->Process(Pixels, Mode, Polarity, Telemetry, FrameIdx);
		}
		// Write ML ground truth annotation and depth (Phase 17)
		if (Collector)
		{
			Collector->WriteAnnotationFrame(Telemetry, FrameIdx);
			if (Depth.Num() > 0)
			{
				Collector->WriteDepthFrame(Depth, CaptureW, CaptureH, FrameIdx);
			}
		}

		// Deposit processed frame into encoder queue (non-blocking)
		FProcessedFrame Frame;
		Frame.Pixels     = MoveTemp(Pixels);
		Frame.Telemetry  = Telemetry;
		Frame.FrameIndex = FrameIdx;
		EncThread->Enqueue(MoveTemp(Frame));

		// Release the game thread to start the next capture — encode runs independently
		bSensorBusy = false;
	});
}
