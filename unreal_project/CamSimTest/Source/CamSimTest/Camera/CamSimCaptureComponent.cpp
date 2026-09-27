// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimCaptureComponent.h"
#include "Health/CamSimSnapshotService.h"
#include "Camera/CamSimPixelConvert.h"
#include "CamSimTest.h"
#include "Config/CamSimConfig.h"
#include "Diagnostics/PipelineLatencyTracker.h"
#include "Encoder/EncoderThread.h"
#include "Encoder/IFrameSink.h"
#include "Entity/CamSimEntityManager.h"
#include "GroundTruth/FEntityProjection.h"
#include "GroundTruth/FGroundTruthCollector.h"
#include "Sensor/SensorPostProcess.h"  // FSensorPostProcess concrete type
#include "Subsystem/CamSimSubsystem.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Engine/Engine.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/Actor.h"
#include "TextureResource.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "PixelFormat.h"
#include "Async/Async.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Kismet/KismetMaterialLibrary.h"

DECLARE_STATS_GROUP(TEXT("CamSim"), STATGROUP_CamSim, STATCAT_Advanced)
DECLARE_CYCLE_STAT(TEXT("Encode Latency"), STAT_CamSimEncode, STATGROUP_CamSim)

// Out-of-line so the header can forward-declare FEncoderThread: every TU that
// destroys the TUniquePtr (including UHT's .gen.cpp) only emits a call here.
void FEncoderThreadDeleter::operator()(FEncoderThread* Ptr) const
{
	delete Ptr;
}

UCamSimCaptureComponent::UCamSimCaptureComponent()
{
	PrimaryComponentTick.bCanEverTick = false;  // driven by ACamSimCamera
}

// -------------------------------------------------------------------------
// Setup / teardown
// -------------------------------------------------------------------------

void UCamSimCaptureComponent::Initialize(USceneCaptureComponent2D* InSensor, UCamSimSubsystem* InSubsystem,
	const FCamSimConfig& Cfg)
{
	Sensor    = InSensor;
	Subsystem = InSubsystem;
	bTrackFrameDrops = Cfg.Performance.bTrackFrameDropsByCategory;
	UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: FrameDropTracking=%s"),
		bTrackFrameDrops ? TEXT("enabled") : TEXT("disabled"));

	// Triple-buffered render targets: frame N+2 renders while N+1 reads back
	// and N encodes (+3.5 MB VRAM at 1280x720).
	RenderTargets.Reset();
	for (int32 Idx = 0; Idx < 3; ++Idx)
	{
		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(this, *FString::Printf(TEXT("CamSimRT_%d"), Idx));
		RT->InitCustomFormat(Cfg.CaptureWidth, Cfg.CaptureHeight, PF_B8G8R8A8, /*bInForceLinearGamma=*/false);
		RT->UpdateResource();
		RenderTargets.Add(RT);
	}
	CaptureTargetIndex = 0;
	PendingReadbackTargetIndex = INDEX_NONE;
	Sensor->TextureTarget = RenderTargets[CaptureTargetIndex];
	Sensor->FOVAngle = Cfg.HFovDeg;

	ColorReadbackPool.Reset();
	for (int32 Idx = 0; Idx < RenderTargets.Num(); ++Idx)
	{
		ColorReadbackPool.Add(MakeUnique<FRHIGPUTextureReadback>(*FString::Printf(TEXT("CamSimReadback_%d"), Idx)));
	}

	// CPU sensor model (Phase 11), behind IPixelPipeline so an alternative
	// implementation can be substituted.
	auto* Pipeline = new FSensorPostProcess();
	Pipeline->Initialize(Cfg.CaptureWidth, Cfg.CaptureHeight, Cfg.SensorModeConfigs, Cfg.ActiveSensorQuality);
	if (Cfg.OpticalRealism.bEnabled && Cfg.OpticalRealism.bLensDistortion)
	{
		Pipeline->SetDistortion(Cfg.OpticalRealism.DistortionK1, Cfg.OpticalRealism.DistortionK2);  // 15B
	}
	Pipeline->SetPhase18Config(Cfg.Phase18);
	Pipeline->SetOverlayConfig(Cfg.OverlayConfig);
	Pipeline->SetLaserDesignatorConfig(Cfg.LaserDesignator);
	SensorFX.Reset(Pipeline);

	ApplyRenderSettings(Cfg);

	if (Cfg.MLTraining.bEnabled && Cfg.MLTraining.bDepthMap)
	{
		CreateDepthCapture(Cfg);
	}

	// Persistent encoder thread (decouples the sensor model from encoding).
	if (IFrameSink* Enc = Subsystem->GetVideoEncoder())
	{
		const float OutputFps = (Cfg.Performance.OutputFrameRateHz > 0.0f) ? Cfg.Performance.OutputFrameRateHz : Cfg.FrameRate;
		// Reset(new …): MakeUnique's default deleter can't convert to ours.
		EncoderThread.Reset(new FEncoderThread(Enc, OutputFps));
		EncoderThread->Start();
		if (LatencyTracker) EncoderThread->SetLatencyTracker(LatencyTracker);
	}
}

void UCamSimCaptureComponent::Shutdown()
{
	// Stop the encoder thread before the subsystem closes the encoder.
	if (EncoderThread)
	{
		EncoderThread->Stop();
		EncoderThread.Reset();
	}

	// No in-flight poll command may touch the pools after we drop them. The
	// tick path never flushes; teardown is the one place we must.
	FlushRenderingCommands();
	ColorReadbackPool.Reset();
	DepthReadbackPool.Reset();
}

void UCamSimCaptureComponent::SetLatencyTracker(FPipelineLatencyTracker* Tracker)
{
	LatencyTracker = Tracker;
	if (EncoderThread) EncoderThread->SetLatencyTracker(Tracker);
}

uint64 UCamSimCaptureComponent::GetDroppedFrameCount() const
{
	return EncoderThread ? EncoderThread->GetDroppedFrameCount() : 0;
}

void UCamSimCaptureComponent::CreateDepthCapture(const FCamSimConfig& Cfg)
{
	DepthCapture = NewObject<USceneCaptureComponent2D>(GetOwner(), TEXT("DepthCapture"));
	DepthCapture->SetupAttachment(GetOwner()->GetRootComponent());
	DepthCapture->bCaptureEveryFrame   = false;
	DepthCapture->bCaptureOnMovement   = false;
	DepthCapture->CaptureSource        = SCS_SceneDepth;
	DepthCapture->bAlwaysPersistRenderingState = false;
	DepthCapture->RegisterComponent();

	DepthRenderTargets.Reset();
	for (int32 Idx = 0; Idx < 2; ++Idx)
	{
		UTextureRenderTarget2D* DRT = NewObject<UTextureRenderTarget2D>(this, *FString::Printf(TEXT("CamSimDepthRT_%d"), Idx));
		DRT->InitCustomFormat(Cfg.CaptureWidth, Cfg.CaptureHeight, PF_R32_FLOAT, /*bInForceLinearGamma=*/true);
		DRT->UpdateResource();
		DepthRenderTargets.Add(DRT);
	}
	DepthCaptureTargetIndex = 0;
	DepthCapture->TextureTarget = DepthRenderTargets[0];

	DepthReadbackPool.Reset();
	for (int32 Idx = 0; Idx < DepthRenderTargets.Num(); ++Idx)
	{
		DepthReadbackPool.Add(MakeUnique<FRHIGPUTextureReadback>(*FString::Printf(TEXT("CamSimDepthReadback_%d"), Idx)));
	}
	UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: depth capture enabled (%dx%d PF_R32_FLOAT, pool=%d)"),
		Cfg.CaptureWidth, Cfg.CaptureHeight, DepthReadbackPool.Num());
}

void UCamSimCaptureComponent::ApplyRenderSettings(const FCamSimConfig& Cfg)
{
	FPostProcessSettings& PP = Sensor->PostProcessSettings;

	// Phase 15 — GPU-side optical realism
	if (Cfg.OpticalRealism.bEnabled)
	{
		const auto& O = Cfg.OpticalRealism;
		Sensor->ShowFlags.SetMotionBlur(O.bMotionBlur);  // 15A
		if (O.bMotionBlur)
		{
			PP.bOverride_MotionBlurAmount = true;
			PP.MotionBlurAmount = O.MotionBlurAmount;
			PP.bOverride_MotionBlurMax = true;
			PP.MotionBlurMax = static_cast<float>(O.MotionBlurMax);
		}
		Sensor->ShowFlags.SetBloom(O.bBloom);  // 15C
		if (O.bBloom)
		{
			PP.bOverride_BloomIntensity = true;
			PP.BloomIntensity = O.BloomIntensity;
			PP.bOverride_BloomThreshold = true;
			PP.BloomThreshold = O.BloomThreshold;
		}
		if (O.bChromaticAberration)  // 15D
		{
			PP.bOverride_SceneFringeIntensity = true;
			PP.SceneFringeIntensity = O.ChromaticAberrationIntensity;
		}
		if (O.bDepthOfField)  // 15E
		{
			PP.bOverride_DepthOfFieldFstop = true;
			PP.DepthOfFieldFstop = O.ApertureFStop;
			PP.bOverride_DepthOfFieldSensorWidth = true;
			PP.DepthOfFieldSensorWidth = O.SensorWidth;
			if (O.FocalDistance > 0.0f)
			{
				PP.bOverride_DepthOfFieldFocalDistance = true;
				PP.DepthOfFieldFocalDistance = O.FocalDistance;
			}
		}
		Sensor->ShowFlags.SetLensFlares(O.bLensFlare);  // 15F
		if (O.bLensFlare)
		{
			PP.bOverride_LensFlareIntensity = true;
			PP.LensFlareIntensity = O.LensFlareIntensity;
			PP.bOverride_LensFlareBokehSize = true;
			PP.LensFlareBokehSize = O.LensFlareBokehSize;
			PP.bOverride_LensFlareThreshold = true;
			PP.LensFlareThreshold = O.LensFlareThreshold;
		}
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: optical realism enabled (blur=%d bloom=%d CA=%d DoF=%d flare=%d distort=%d)"),
			O.bMotionBlur, O.bBloom, O.bChromaticAberration, O.bDepthOfField, O.bLensFlare, O.bLensDistortion);
	}

	// Phase 24 — rendering quality
	{
		const FCamSimConfig::FRenderingQualityConfig& RQ = Cfg.RenderingQuality;
		Sensor->ShowFlags.SetContactShadows(RQ.bContactShadows);  // 24A (per-light length set in editor)
		if (RQ.AOIntensity > 0.0f)  // 24B
		{
			PP.bOverride_AmbientOcclusionIntensity = true;
			PP.AmbientOcclusionIntensity           = RQ.AOIntensity;
			PP.bOverride_AmbientOcclusionRadius    = true;
			PP.AmbientOcclusionRadius              = RQ.AORadius;
		}

		auto SetCVar = [](const TCHAR* Name, float Value)
		{
			if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name)) CVar->Set(Value, ECVF_SetByCode);
		};
		auto SetCVarI = [](const TCHAR* Name, int32 Value)
		{
			if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name)) CVar->Set(Value, ECVF_SetByCode);
		};
		SetCVarI(TEXT("r.RayTracing"),             RQ.bRayTracingEnabled ? 1 : 0);
		SetCVarI(TEXT("r.RayTracing.Reflections"), RQ.bRayTracedReflections ? 1 : 0);  // 24D
		SetCVar (TEXT("r.Shadow.DistanceScale"),                        RQ.ShadowDistanceScale);  // 24E
		SetCVarI(TEXT("r.Shadow.Virtual.ResolutionLodBiasDirectional"), RQ.VSMResolutionBias);
		SetCVarI(TEXT("r.Shadow.Virtual.MaxPhysicalPages"),             RQ.VSMMaxPhysicalPages);
		if (RQ.TSRScreenPercentage != 100)  // 24F
		{
			SetCVarI(TEXT("r.ScreenPercentage"), RQ.TSRScreenPercentage);
		}
		// FXAA (1): TSR (4) ghosts on off-screen captures; FXAA is stateless.
		SetCVarI(TEXT("r.AntiAliasingMethod"), 1);

		UE_LOG(LogCamSim, Log,
			TEXT("ACamSimCamera: RenderingQuality — shadows=%d contactShadow=%d AO=%.2f "
			     "RTRefl=%d shadowDist=%.1f VSMBias=%d TSR%%=%d AA=FXAA"),
			(int)RQ.bEntityShadows, (int)RQ.bContactShadows, RQ.AOIntensity,
			(int)RQ.bRayTracedReflections, RQ.ShadowDistanceScale,
			RQ.VSMResolutionBias, RQ.TSRScreenPercentage);
	}

	// 27F — configurable render frame rate
	const float TargetRenderFps = FMath::Clamp(Cfg.Performance.RenderFrameRateHz, 1.0f, 120.0f);
	if (!FMath::IsNearlyEqual(TargetRenderFps, 30.0f))
	{
		GEngine->SetMaxFPS(TargetRenderFps);
		GEngine->FixedFrameRate     = TargetRenderFps;
		GEngine->bUseFixedFrameRate = true;
	}

	// 27G — texture streaming pool budget
	if (Cfg.Performance.TexturePoolBudgetMB > 0)
	{
		GEngine->Exec(GetWorld(), *FString::Printf(TEXT("r.Streaming.PoolSize %d"), Cfg.Performance.TexturePoolBudgetMB));
	}

	UE_LOG(LogCamSim, Log,
		TEXT("ACamSimCamera: Performance — renderFPS=%.0f outputFPS=%.0f texturePoolMB=%d dropTracking=%s hotReload=%s"),
		TargetRenderFps, Cfg.Performance.OutputFrameRateHz, Cfg.Performance.TexturePoolBudgetMB,
		Cfg.Performance.bTrackFrameDropsByCategory ? TEXT("1") : TEXT("0"),
		Cfg.Performance.bHotReloadConfig ? TEXT("1") : TEXT("0"));

	// 27A — GPU sensor post-process material
	if (Cfg.Performance.bGpuSensorEffects)
	{
		GpuSensorMat = Cast<UMaterialInterface>(StaticLoadObject(UMaterialInterface::StaticClass(), nullptr,
			*Cfg.Performance.GpuSensorMaterialPath));
		GpuSensorMpc = Cast<UMaterialParameterCollection>(StaticLoadObject(UMaterialParameterCollection::StaticClass(),
			nullptr, *Cfg.Performance.GpuSensorMpcPath));
		if (GpuSensorMat && GpuSensorMpc)
		{
			FWeightedBlendable Blendable;
			Blendable.Object = GpuSensorMat;
			Blendable.Weight = 1.0f;
			PP.WeightedBlendables.Array.Add(Blendable);

			// Bypass the expensive CPU loops — defect pixels, quantization and overlay still run.
			SensorFX->SetGpuSensorEffectsActive(true);
			UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: GPU sensor pipeline ACTIVE — material=%s"),
				*Cfg.Performance.GpuSensorMaterialPath);
		}
		else
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("ACamSimCamera: GPU sensor material/MPC not found at '%s' / '%s' — falling back to CPU pipeline"),
				*Cfg.Performance.GpuSensorMaterialPath, *Cfg.Performance.GpuSensorMpcPath);
		}
	}
}

void UCamSimCaptureComponent::UpdateGpuSensorParams(ESensorMode Mode, const FCamSimConfig& Cfg)
{
	if (!GpuSensorMpc || !GetWorld()) return;
	const FSensorModeConfig* IrCfg = Cfg.SensorModeConfigs.Find(ESensorMode::IR);
	UKismetMaterialLibrary::SetScalarParameterValue(GetWorld(), GpuSensorMpc, TEXT("SensorMode"), static_cast<float>(Mode));
	UKismetMaterialLibrary::SetScalarParameterValue(GetWorld(), GpuSensorMpc, TEXT("NoiseIntensity"), IrCfg ? IrCfg->NETD : 0.0f);
}

bool UCamSimCaptureComponent::ShouldSkipFrameForDecimation(const FCamSimConfig& Cfg)
{
	const float RenderFps = Cfg.Performance.RenderFrameRateHz;
	const float OutputFps = Cfg.Performance.OutputFrameRateHz;
	if (RenderFps <= OutputFps || OutputFps <= 0.0f) return false;

	RenderFrameCounter++;
	const uint64 DecimationRatio = FMath::RoundToInt64(RenderFps / OutputFps);
	return DecimationRatio > 1 && (RenderFrameCounter % DecimationRatio) != 0;
}

// -------------------------------------------------------------------------
// Capture — trigger the GPU capture and enqueue the async readback
// -------------------------------------------------------------------------
// Poll() completes it on later ticks. Frame N+1 renders on the game thread
// while the render thread finishes frame N's DMA and the sensor task runs.

bool UCamSimCaptureComponent::IsReadyForCapture() const
{
	// Idle: no DMA in flight and no completed result awaiting consumption.
	return ReadbackState.Load(EMemoryOrder::SequentiallyConsistent) == EReadbackState::Idle;
}

void UCamSimCaptureComponent::SnapshotGroundTruthEntities()
{
	FGroundTruthCollector* Collector = Subsystem->GetGroundTruthCollector();
	FCamSimEntityManager*  EntityMgr = Subsystem->GetEntityManager();
	if (!Collector || !Collector->IsEnabled() || !EntityMgr) return;

	const FCamSimConfig& Cfg = Subsystem->GetConfig();
	FViewProjectionData ViewProj;
	ViewProj.ViewProjectionMatrix = FEntityProjection::BuildViewProjectionMatrix(
		Sensor->GetComponentLocation(), Sensor->GetComponentRotation(), Sensor->FOVAngle,
		Cfg.CaptureWidth, Cfg.CaptureHeight);
	ViewProj.ImageWidth  = Cfg.CaptureWidth;
	ViewProj.ImageHeight = Cfg.CaptureHeight;

	// Cheap cone-cull hint, ~30% wider than the HFOV so entities barely
	// outside still get projected (the AABB projection clips correctly).
	ViewProj.CameraLocation = Sensor->GetComponentLocation();
	ViewProj.CameraForward  = Sensor->GetForwardVector();
	ViewProj.CullConeHalfAngleCos = FMath::Cos(FMath::DegreesToRadians(FMath::Min(90.0f, Sensor->FOVAngle * 0.65f)));

	Collector->SetPendingEntitySnapshot(EntityMgr->GetEntitySnapshot(ViewProj), Cfg.CaptureWidth, Cfg.CaptureHeight);
}

void UCamSimCaptureComponent::Capture(const FCamSimTelemetry& Telemetry)
{
	if (!Sensor) return;
	if (!RenderTargets.IsValidIndex(CaptureTargetIndex) || !RenderTargets[CaptureTargetIndex]) return;
	if (!ColorReadbackPool.IsValidIndex(CaptureTargetIndex) || !ColorReadbackPool[CaptureTargetIndex]) return;

	// Entity annotation snapshot for this exact view (Phase 17D).
	if (Subsystem) SnapshotGroundTruthEntities();

	UTextureRenderTarget2D* RT = RenderTargets[CaptureTargetIndex].Get();
	FRHIGPUTextureReadback* Readback = ColorReadbackPool[CaptureTargetIndex].Get();
	Sensor->TextureTarget = RT;
	Sensor->CaptureScene();
	PendingReadbackTargetIndex = CaptureTargetIndex;
	CaptureTargetIndex = (CaptureTargetIndex + 1) % RenderTargets.Num();

	PendingFrameIndex = FrameIndex++;
	PendingTelemetry  = Telemetry;

	// Enter DMAQueued and bump the poll generation so stale polls from the
	// previous frame no-op.
	PollGeneration.Store(PollGeneration.Load(EMemoryOrder::Relaxed) + 1, EMemoryOrder::SequentiallyConsistent);
	RenderReadyStreak     .Store(0, EMemoryOrder::Relaxed);
	RenderDepthReadyStreak.Store(0, EMemoryOrder::Relaxed);
	ReadbackState.Store(EReadbackState::DMAQueued, EMemoryOrder::SequentiallyConsistent);

	// Depth alongside color (Phase 17A)
	UTextureRenderTarget2D* DepthRT = nullptr;
	FRHIGPUTextureReadback* DepthReadback = nullptr;
	PendingDepthReadbackTargetIndex = INDEX_NONE;
	if (DepthCapture && DepthRenderTargets.IsValidIndex(DepthCaptureTargetIndex))
	{
		DepthRT = DepthRenderTargets[DepthCaptureTargetIndex].Get();
		DepthCapture->TextureTarget = DepthRT;
		DepthCapture->SetRelativeRotation(Sensor->GetRelativeRotation());  // align with color
		DepthCapture->CaptureScene();
		PendingDepthReadbackTargetIndex = DepthCaptureTargetIndex;
		DepthReadback = DepthReadbackPool.IsValidIndex(DepthCaptureTargetIndex)
			? DepthReadbackPool[DepthCaptureTargetIndex].Get() : nullptr;
		DepthCaptureTargetIndex = (DepthCaptureTargetIndex + 1) % DepthRenderTargets.Num();
	}

	// Async GPU→CPU DMA on the render thread (returns immediately).
	ENQUEUE_RENDER_COMMAND(CamSimEnqueueReadback)(
		[RT, Readback, DepthRT, DepthReadback](FRHICommandListImmediate& RHICmdList)
	{
		FTextureRenderTargetResource* Resource = RT->GetRenderTargetResource();
		if (!Resource) return;
		FRHITexture* SourceTexture = Resource->GetRenderTargetTexture();

		// Explicit barriers: Metal tracks resources implicitly, Vulkan does not.
		RHICmdList.Transition(FRHITransitionInfo(SourceTexture, ERHIAccess::RTV, ERHIAccess::CopySrc));
		Readback->EnqueueCopy(RHICmdList, SourceTexture);  // non-blocking; Poll() checks IsReady()
		RHICmdList.Transition(FRHITransitionInfo(SourceTexture, ERHIAccess::CopySrc, ERHIAccess::RTV));

		if (DepthRT && DepthReadback)
		{
			FTextureRenderTargetResource* DepthRes = DepthRT->GetRenderTargetResource();
			if (FRHITexture* DepthTex = DepthRes ? DepthRes->GetRenderTargetTexture() : nullptr)
			{
				RHICmdList.Transition(FRHITransitionInfo(DepthTex, ERHIAccess::RTV, ERHIAccess::CopySrc));
				DepthReadback->EnqueueCopy(RHICmdList, DepthTex);
				RHICmdList.Transition(FRHITransitionInfo(DepthTex, ERHIAccess::CopySrc, ERHIAccess::RTV));
			}
		}
		// State stays DMAQueued; the poll command reads ReadbackState directly.
	});
}

// -------------------------------------------------------------------------
// Poll — complete the readback (no FlushRenderingCommands)
// -------------------------------------------------------------------------

void UCamSimCaptureComponent::Poll()
{
	checkSlow(IsInGameThread());  // reads game-thread-only state

	const EReadbackState State = ReadbackState.Load(EMemoryOrder::SequentiallyConsistent);
	if (State == EReadbackState::Complete)
	{
		if (LatencyTracker) LatencyTracker->Mark(EPipelineStage::ReadbackComplete);

		TArray<FColor> Pixels = MoveTemp(AsyncPixels);

		// ROADMAP 3A reference shots: the pre-sensor frame, lossless.
		if (FCamSimSnapshotService* Snap = Subsystem ? Subsystem->GetSnapshotService() : nullptr)
		{
			if (Snap->WantsFrame())
			{
				const FCamSimConfig& SnapCfg = Subsystem->GetConfig();
				Snap->OfferFrame(Pixels, SnapCfg.CaptureWidth, SnapCfg.CaptureHeight);
			}
		}
		TArray<float>  Depth  = MoveTemp(AsyncDepth);
		ReadbackState.Store(EReadbackState::Idle, EMemoryOrder::SequentiallyConsistent);
		PendingReadbackTargetIndex      = INDEX_NONE;
		PendingDepthReadbackTargetIndex = INDEX_NONE;

		if (!bSensorBusy)
		{
			bSensorBusy = true;
			SubmitFrameToEncoder(MoveTemp(Pixels), PendingTelemetry, PendingFrameIndex, MoveTemp(Depth));
		}
		else
		{
			// Hold for dispatch when the sensor task frees up.
			CompletedPixels      = MoveTemp(Pixels);
			CompletedDepth       = MoveTemp(Depth);
			CompletedTelemetry   = PendingTelemetry;
			CompletedFrameIndex  = PendingFrameIndex;
			bReadbackResultReady = true;
		}
	}
	else if (State == EReadbackState::Failed)
	{
		if (bTrackFrameDrops) FrameDropStats.ReadbackTimeout++;
		UE_LOG(LogCamSim, Warning, TEXT("CamSimReadback frame %llu: lock returned null or bad format"), PendingFrameIndex);
		AsyncPixels.Reset();
		AsyncDepth.Reset();
		ReadbackState.Store(EReadbackState::Idle, EMemoryOrder::SequentiallyConsistent);
		PendingReadbackTargetIndex      = INDEX_NONE;
		PendingDepthReadbackTargetIndex = INDEX_NONE;
	}
	else if (State == EReadbackState::DMAQueued)
	{
		EnqueuePoll();
	}

	// A held result goes out as soon as the sensor task is free.
	if (bReadbackResultReady && !bSensorBusy)
	{
		bSensorBusy = true;
		SubmitFrameToEncoder(MoveTemp(CompletedPixels), CompletedTelemetry, CompletedFrameIndex, MoveTemp(CompletedDepth));
		bReadbackResultReady = false;
	}
}

void UCamSimCaptureComponent::EnqueuePoll()
{
	// A non-blocking render command polls IsReady(); on success it copies the
	// pixels into AsyncPixels and moves ReadbackState to Complete (SeqCst). The
	// game thread consumes it on a later tick, so the render thread keeps
	// rendering frame N+1 while frame N's DMA drains. Everything the lambda
	// needs is captured by value.
	const FCamSimConfig& Cfg = Subsystem->GetConfig();
	const int32 ReadyPollsRequired = FMath::Max(1, Cfg.ReadbackReadyPolls);
	FRHIGPUTextureReadback* Readback = ColorReadbackPool.IsValidIndex(PendingReadbackTargetIndex)
		? ColorReadbackPool[PendingReadbackTargetIndex].Get() : nullptr;
	FRHIGPUTextureReadback* DepthReadback = DepthReadbackPool.IsValidIndex(PendingDepthReadbackTargetIndex)
		? DepthReadbackPool[PendingDepthReadbackTargetIndex].Get() : nullptr;
	UTextureRenderTarget2D* RT = RenderTargets.IsValidIndex(PendingReadbackTargetIndex)
		? RenderTargets[PendingReadbackTargetIndex].Get() : nullptr;

	if (LatencyTracker) LatencyTracker->Mark(EPipelineStage::ReadbackIssue);

	const int32  CaptureW       = Cfg.CaptureWidth;
	const int32  CaptureH       = Cfg.CaptureHeight;
	const auto   ReadbackFormat = Cfg.ReadbackFormat;
	const bool   bSwapRB        = Cfg.bSwapRBReadback;
	const uint64 FrameIdx       = PendingFrameIndex;
	const uint32 CaptureGen     = PollGeneration.Load(EMemoryOrder::Relaxed);

	ENQUEUE_RENDER_COMMAND(CamSimPollReadback)(
		[this, Readback, DepthReadback, RT, ReadyPollsRequired,
		 CaptureW, CaptureH, ReadbackFormat, bSwapRB, FrameIdx, CaptureGen]
		(FRHICommandListImmediate&)
	{
		// Stale poll from a previous frame — the game thread has moved on.
		if (PollGeneration.Load(EMemoryOrder::Relaxed) != CaptureGen) return;
		// Only in DMAQueued: an earlier poll on this frame may already have delivered.
		if (ReadbackState.Load(EMemoryOrder::Relaxed) != EReadbackState::DMAQueued) return;

		if (!Readback || !Readback->IsReady())
		{
			RenderReadyStreak.Store(0, EMemoryOrder::Relaxed);
			return;
		}
		{
			const uint8 Cur = RenderReadyStreak.Load(EMemoryOrder::Relaxed);
			if (Cur < 255) RenderReadyStreak.Store(Cur + 1, EMemoryOrder::Relaxed);
		}
		if (RenderReadyStreak.Load(EMemoryOrder::Relaxed) < ReadyPollsRequired) return;

		int32 RowPitch = 0;
		void* RawData = Readback->Lock(RowPitch);
		if (!RawData || !RT)
		{
			if (RawData) Readback->Unlock();
			ReadbackState.Store(EReadbackState::Failed, EMemoryOrder::SequentiallyConsistent);
			return;
		}

		const int32 W = RT->SizeX;
		const int32 H = RT->SizeY;
		const EPixelFormat PixelFormat = RT->GetFormat();
		if (PixelFormat != PF_B8G8R8A8 && PixelFormat != PF_R8G8B8A8)
		{
			Readback->Unlock();
			ReadbackState.Store(EReadbackState::Failed, EMemoryOrder::SequentiallyConsistent);
			return;
		}

		AsyncPixels.SetNumUninitialized(W * H);
		CamSimConvertReadbackPixels(RawData, RowPitch, W, H, PixelFormat, ReadbackFormat, bSwapRB, AsyncPixels, FrameIdx);
		Readback->Unlock();

		// Opportunistic depth: it may lag color; skip this frame's if so.
		AsyncDepth.Reset();
		if (DepthReadback && DepthReadback->IsReady())
		{
			{
				const uint8 Cur = RenderDepthReadyStreak.Load(EMemoryOrder::Relaxed);
				if (Cur < 255) RenderDepthReadyStreak.Store(Cur + 1, EMemoryOrder::Relaxed);
			}
			if (RenderDepthReadyStreak.Load(EMemoryOrder::Relaxed) >= ReadyPollsRequired)
			{
				int32 DepthRowPitch = 0;
				if (void* DepthRaw = DepthReadback->Lock(DepthRowPitch))  // null: do NOT Unlock (UB)
				{
					AsyncDepth.SetNumUninitialized(CaptureW * CaptureH);
					const uint8* Src = static_cast<const uint8*>(DepthRaw);
					float*       Dst = AsyncDepth.GetData();
					for (int32 Row = 0; Row < CaptureH; ++Row)
					{
						FMemory::Memcpy(Dst + Row * CaptureW, Src + Row * DepthRowPitch, CaptureW * sizeof(float));
					}
					for (float& V : AsyncDepth) { V /= 100.0f; }  // cm -> m
					DepthReadback->Unlock();
				}
			}
		}
		else
		{
			RenderDepthReadyStreak.Store(0, EMemoryOrder::Relaxed);
		}

		// SeqCst: the array writes above are visible once the game thread sees Complete.
		ReadbackState.Store(EReadbackState::Complete, EMemoryOrder::SequentiallyConsistent);
	});
}

// -------------------------------------------------------------------------
// SubmitFrameToEncoder — sensor model on a pool thread, then the encoder queue
// -------------------------------------------------------------------------

void UCamSimCaptureComponent::SubmitFrameToEncoder(
	TArray<FColor> PixelData, FCamSimTelemetry Telemetry, uint64 FrameIdx, TArray<float> DepthMetres)
{
	if (!EncoderThread)
	{
		bSensorBusy = false;
		return;
	}

	// Sensor state comes from the telemetry snapshot: UObjects are game-thread only.
	const ESensorMode Mode     = static_cast<ESensorMode>(Telemetry.SensorMode);
	const uint8       Polarity = Telemetry.SensorPolarity;

	// With GPU sensor effects active (27A), Process() skips the expensive CPU
	// loops and only runs defect pixels, quantization and overlay.
	IPixelPipeline*          FX        = SensorFX.Get();
	FGroundTruthCollector*   Collector = Subsystem ? Subsystem->GetGroundTruthCollector() : nullptr;
	const int32              CaptureW  = Subsystem ? Subsystem->GetConfig().CaptureWidth  : 0;
	const int32              CaptureH  = Subsystem ? Subsystem->GetConfig().CaptureHeight : 0;
	FEncoderThread*          EncThread = EncoderThread.Get();  // outlives the task
	FPipelineLatencyTracker* LT        = LatencyTracker;       // outlives the task

	// bSensorBusy is cleared after the sensor model + ML annotation, not after encode.
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
		[this, EncThread, FX, Mode, Polarity, Collector, CaptureW, CaptureH, LT,
		 Pixels = MoveTemp(PixelData), Telemetry, FrameIdx, Depth = MoveTemp(DepthMetres)]() mutable
	{
		SCOPE_CYCLE_COUNTER(STAT_CamSimEncode);
		if (LT) LT->Mark(EPipelineStage::SensorStart);
		if (FX)
		{
			FX->Process(Pixels, Mode, Polarity, Telemetry, FrameIdx);
		}
		if (LT) LT->Mark(EPipelineStage::SensorEnd);

		// ML ground truth annotation and depth (Phase 17)
		if (Collector)
		{
			Collector->WriteAnnotationFrame(Telemetry, FrameIdx);
			if (Depth.Num() > 0)
			{
				Collector->WriteDepthFrame(Depth, CaptureW, CaptureH, FrameIdx);
			}
		}

		FProcessedFrame Frame;
		Frame.Pixels     = MoveTemp(Pixels);
		Frame.Telemetry  = Telemetry;
		Frame.FrameIndex = FrameIdx;
		EncThread->Enqueue(MoveTemp(Frame));  // non-blocking

		bSensorBusy = false;  // the next capture can start; encode runs independently
	});
}
