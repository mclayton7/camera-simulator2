// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimCaptureComponent.h"
#include "Health/CamSimSnapshotService.h"
#include "Camera/CamSimFrameGrabExtension.h"
#include "Camera/CamSimRenderPath.h"
#include "Engine/GameViewportClient.h"
#include "SceneViewExtension.h"
#include "Camera/CamSimPixelConvert.h"
#include "CamSimTest.h"
#include "Config/CamSimConfig.h"
#include "Diagnostics/PipelineLatencyTracker.h"
#include "Encoder/EncoderThread.h"
#include "Encoder/IFrameSink.h"
#include "Encoder/Nv12.h"
#include "Entity/CamSimEntityManager.h"
#include "GroundTruth/FEntityProjection.h"
#include "GroundTruth/FGroundTruthCollector.h"
#include "Sensor/SensorPostProcess.h"  // FSensorPostProcess concrete type
#include "Subsystem/CamSimSubsystem.h"
#include "Time/SimClock.h"

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

	// One render target per readback slot: up to three frames in flight
	// (+3.5 MB VRAM at 1280x720).
	RenderTargets.Reset();
	for (int32 Idx = 0; Idx < FReadbackRing::NumSlots; ++Idx)
	{
		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(this, *FString::Printf(TEXT("CamSimRT_%d"), Idx));
		RT->InitCustomFormat(Cfg.CaptureWidth, Cfg.CaptureHeight, PF_B8G8R8A8, /*bInForceLinearGamma=*/false);
		RT->UpdateResource();
		RenderTargets.Add(RT);
	}
	Sensor->TextureTarget = RenderTargets[0];
	Sensor->FOVAngle = Cfg.HFovDeg;

	bPrimaryView = Cfg.Render.IsPrimary();

	// ROADMAP 3B: the sensor pipeline is chosen once per session, by the
	// subsystem (it logs the reason), so the encoder's transfer tag agrees.
	SensorPath = Subsystem->GetSensorPathDecision();
	bGpuSensor = SensorPath.Path == ESensorPipelinePath::Gpu;
	UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: %s"), *SensorPath.Reason);
	if (bGpuSensor)
	{
		GpuSensorSize = FIntPoint(Cfg.CaptureWidth, Cfg.CaptureHeight);
		Nv12ReadbackPool.Reset();
		for (int32 Idx = 0; Idx < FReadbackRing::NumSlots; ++Idx)
		{
			Nv12ReadbackPool.Add(MakeUnique<FRHIGPUBufferReadback>(*FString::Printf(TEXT("CamSimNv12Readback_%d"), Idx)));
		}
	}

	for (TAtomic<uint32>& Gen : GrabbedGeneration) { Gen.Store(0); }
	if (bPrimaryView && !EnsureGrabExtension())
	{
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: primary view — game viewport not created yet; grabbing starts when it is"));
	}

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

bool UCamSimCaptureComponent::EnsureGrabExtension()
{
	if (GrabExtension) return true;
	FViewport* Viewport = (GEngine && GEngine->GameViewport) ? GEngine->GameViewport->Viewport : nullptr;
	if (!Viewport) return false;
	GrabExtension = FSceneViewExtensions::NewExtension<FCamSimFrameGrabExtension>(Viewport);
	if (bGpuSensor)
	{
		GrabExtension->EnableGpuSensor_GameThread(GpuSensorSize, &StatsMailbox);
	}
	UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: primary view — grabbing the game viewport%s"),
		bGpuSensor ? TEXT(" (GPU sensor replaces the tonemapper)") : TEXT(""));
	return true;
}

void UCamSimCaptureComponent::Shutdown()
{
	// Stop the encoder thread before the subsystem closes the encoder.
	if (EncoderThread)
	{
		EncoderThread->Stop();
		EncoderThread.Reset();
	}

	if (GrabExtension) { GrabExtension->Detach_GameThread(); }

	// No in-flight poll command may touch the pools after we drop them. The
	// tick path never flushes; teardown is the one place we must.
	FlushRenderingCommands();
	GrabExtension.Reset();
	ColorReadbackPool.Reset();
	Nv12ReadbackPool.Reset();
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
	for (int32 Idx = 0; Idx < FReadbackRing::NumSlots; ++Idx)
	{
		UTextureRenderTarget2D* DRT = NewObject<UTextureRenderTarget2D>(this, *FString::Printf(TEXT("CamSimDepthRT_%d"), Idx));
		DRT->InitCustomFormat(Cfg.CaptureWidth, Cfg.CaptureHeight, PF_R32_FLOAT, /*bInForceLinearGamma=*/true);
		DRT->UpdateResource();
		DepthRenderTargets.Add(DRT);
	}
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
	// Primary view: the game viewport's show flags get the same values as the capture's.
	FEngineShowFlags* ViewFlags = (Cfg.Render.IsPrimary() && GEngine && GEngine->GameViewport)
		? &GEngine->GameViewport->EngineShowFlags : nullptr;
	if (Cfg.Render.IsPrimary())
	{
		// The game viewport's canvas (map warnings, renderer notices such as
		// Lumen's exposure-range warning, debug messages) would be burned into
		// the grabbed frame. Screenshots and movie dumps suppress it the same way.
		GAreScreenMessagesEnabled = false;
	}

	// 15A motion blur: always explicit (off unless optical realism enables it),
	// on both the capture and the primary view.
	CamSimRender::ApplyMotionBlur(Cfg.OpticalRealism, PP, Sensor->ShowFlags);
	if (ViewFlags) ViewFlags->SetMotionBlur(Sensor->ShowFlags.MotionBlur != 0);

	// Phase 15 — GPU-side optical realism
	if (Cfg.OpticalRealism.bEnabled)
	{
		const auto& O = Cfg.OpticalRealism;
		Sensor->ShowFlags.SetBloom(O.bBloom);
		if (ViewFlags) ViewFlags->SetBloom(O.bBloom);  // 15C
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
		Sensor->ShowFlags.SetLensFlares(O.bLensFlare);
		if (ViewFlags) ViewFlags->SetLensFlares(O.bLensFlare);  // 15F
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

	if (bGpuSensor)
	{
		// ROADMAP 3B: the sensor owns exposure. UE runs manual so its eye adaptation
		// never fights the sensor AE; UpdateSensorParams sets the bias each tick only
		// to keep scene colour in fp16 range (the graph divides PreExposure back out).
		PP.bOverride_AutoExposureMethod = true;
		PP.AutoExposureMethod = AEM_Manual;
		PP.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
		PP.AutoExposureApplyPhysicalCameraExposure = false;
		PP.bOverride_AutoExposureBias = true;
		PP.AutoExposureBias = 0.0f;
	}
	else
	{
		// Auto-exposure stays on; the compensation shifts where it settles. Set on
		// the SceneCapture's post-process, which the primary view mirrors.
		const IConsoleVariable* DefaultBias = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DefaultFeature.AutoExposure.Bias"));
		PP.bOverride_AutoExposureBias = true;
		PP.AutoExposureBias = CamSimRender::AutoExposureBias(DefaultBias ? DefaultBias->GetFloat() : 1.0f,
			Cfg.Render.ExposureCompensationEV);
	}

	// Phase 24 — rendering quality
	{
		const FCamSimConfig::FRenderingQualityConfig& RQ = Cfg.RenderingQuality;
		Sensor->ShowFlags.SetContactShadows(RQ.bContactShadows);
		if (ViewFlags) ViewFlags->SetContactShadows(RQ.bContactShadows);  // 24A (per-light length set in editor)
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
		// Primary view: TSR (4) with full view history. SceneCapture: FXAA (1),
		// because TSR ghosts on off-screen captures (no persistent history).
		SetCVarI(TEXT("r.AntiAliasingMethod"), Cfg.Render.IsPrimary() ? 4 : 1);

		UE_LOG(LogCamSim, Log,
			TEXT("ACamSimCamera: RenderingQuality — shadows=%d contactShadow=%d AO=%.2f "
			     "RTRefl=%d shadowDist=%.1f VSMBias=%d TSR%%=%d AA=%s"),
			(int)RQ.bEntityShadows, (int)RQ.bContactShadows, RQ.AOIntensity,
			(int)RQ.bRayTracedReflections, RQ.ShadowDistanceScale,
			RQ.VSMResolutionBias, RQ.TSRScreenPercentage, Cfg.Render.IsPrimary() ? TEXT("TSR") : TEXT("FXAA"));
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

void UCamSimCaptureComponent::UpdateSensorParams(ESensorMode Mode, uint8 Polarity, bool bCameraCut, const FCamSimConfig& Cfg)
{
	if (!bGpuSensor || !GrabExtension) return;

	const double NowSimSec = static_cast<double>(FSimClock::Get().NowMicros()) * 1e-6;
	const double Dt = LastSensorUpdateSimSec < 0.0 ? 0.0 : NowSimSec - LastSensorUpdateSimSec;
	LastSensorUpdateSimSec = NowSimSec;

	FSensorHistogram Hist;
	const bool bHasHist = StatsMailbox.TakeLatest(Hist);

	FSensorControllerInput In;
	In.Mode         = static_cast<ESensorGraphMode>(FMath::Clamp(static_cast<int32>(Mode), 0, 2));
	In.bBlackHot    = Polarity != 0;
	In.bCameraCut   = bCameraCut;
	In.DeltaSimSec  = Dt;
	In.NewHistogram = bHasHist ? &Hist : nullptr;
	In.Serial       = ++ParamsSerial;
	const FSensorModeConfig* ModeCfg = Cfg.SensorModeConfigs.Find(Mode);
	const uint32 StaleBefore = SensorController.GetStaleEpisodes();
	const FSensorFrameParams Params = SensorController.Update(In, ModeCfg ? *ModeCfg : FSensorModeConfig());
	if (SensorController.GetStaleEpisodes() != StaleBefore && bTrackFrameDrops) FrameDropStats.SensorStatsStale++;

	// Keep UE's pre-exposure near the sensor gain so scene colour stays in fp16 range.
	Sensor->PostProcessSettings.AutoExposureBias = SensorController.GetGainEv() + UeExposureOffsetEv;

	TSharedPtr<FCamSimFrameGrabExtension, ESPMode::ThreadSafe> Ext = GrabExtension;
	ENQUEUE_RENDER_COMMAND(CamSimSensorParams)([Ext, Params](FRHICommandListImmediate&)
	{
		Ext->SetParams_RenderThread(Params);
	});
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
	return Ring.CanAcquire();
}

void UCamSimCaptureComponent::NoteCaptureSkipped()
{
	// Every slot is in flight or waiting for the sensor task: downstream is behind.
	if (bTrackFrameDrops) FrameDropStats.EncoderBusy++;
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
	if (bPrimaryView && !EnsureGrabExtension()) return;  // nothing to grab yet
	if (!Ring.CanAcquire()) { NoteCaptureSkipped(); return; }

	const uint64 FrameIdx = FrameIndex;
	const int32  Slot     = Ring.Acquire(FrameIdx);
	if (!RenderTargets.IsValidIndex(Slot) || !RenderTargets[Slot]
		|| !ColorReadbackPool.IsValidIndex(Slot) || !ColorReadbackPool[Slot])
	{
		Ring.MarkFailed(Slot);  // released by Poll() like any failed readback
		return;
	}
	++FrameIndex;

	// Entity annotation snapshot for this exact view (Phase 17D).
	if (Subsystem) SnapshotGroundTruthEntities();

	FSlot& S = Slots[Slot];
	const uint32 Gen = ++NextGeneration;
	S.Telemetry = Telemetry;
	S.ReadyStreak     .Store(0, EMemoryOrder::Relaxed);
	S.DepthReadyStreak.Store(0, EMemoryOrder::Relaxed);
	S.PollAttempts    .Store(0, EMemoryOrder::Relaxed);
	// SeqCst: the resets above are visible to any poll that sees the new generation.
	S.Generation.Store(Gen, EMemoryOrder::SequentiallyConsistent);

	UTextureRenderTarget2D* RT = RenderTargets[Slot].Get();
	FRHIGPUTextureReadback* Readback = ColorReadbackPool[Slot].Get();
	if (!bPrimaryView)
	{
		Sensor->TextureTarget = RT;
		Sensor->CaptureScene();
	}

	// Depth alongside color (Phase 17A), in the same slot.
	UTextureRenderTarget2D* DepthRT = nullptr;
	FRHIGPUTextureReadback* DepthReadback = nullptr;
	if (DepthCapture && DepthRenderTargets.IsValidIndex(Slot))
	{
		DepthRT = DepthRenderTargets[Slot].Get();
		DepthCapture->TextureTarget = DepthRT;
		DepthCapture->SetRelativeRotation(Sensor->GetRelativeRotation());  // align with color
		DepthCapture->CaptureScene();
		DepthReadback = DepthReadbackPool.IsValidIndex(Slot) ? DepthReadbackPool[Slot].Get() : nullptr;
	}

	auto EnqueueDepthCopy = [](FRHICommandListImmediate& RHICmdList, UTextureRenderTarget2D* DRT, FRHIGPUTextureReadback* DRB)
	{
		if (!DRT || !DRB) return;
		FTextureRenderTargetResource* DepthRes = DRT->GetRenderTargetResource();
		if (FRHITexture* DepthTex = DepthRes ? DepthRes->GetRenderTargetTexture() : nullptr)
		{
			RHICmdList.Transition(FRHITransitionInfo(DepthTex, ERHIAccess::RTV, ERHIAccess::CopySrc));
			DRB->EnqueueCopy(RHICmdList, DepthTex);
			RHICmdList.Transition(FRHITransitionInfo(DepthTex, ERHIAccess::CopySrc, ERHIAccess::RTV));
		}
	};

	if (bPrimaryView)
	{
		// The game viewport renders after this tick; the extension copies that
		// frame into RT and queues the readback. Enqueued now, so it's the newest
		// request when this frame's scene render runs.
		// GPU sensor (3B): the sensor graph's NV12 output is read back instead.
		TSharedPtr<FCamSimFrameGrabExtension, ESPMode::ThreadSafe> Ext = GrabExtension;
		TAtomic<uint32>* Grabbed = &GrabbedGeneration[Slot];
		FRHIGPUBufferReadback* Nv12Readback = (bGpuSensor && Nv12ReadbackPool.IsValidIndex(Slot)) ? Nv12ReadbackPool[Slot].Get() : nullptr;
		const bool bGpu = bGpuSensor;
		ENQUEUE_RENDER_COMMAND(CamSimRequestGrab)(
			[Ext, RT, Readback, Nv12Readback, bGpu, Gen, FrameIdx, Slot, Grabbed, DepthRT, DepthReadback, EnqueueDepthCopy](FRHICommandListImmediate& RHICmdList)
		{
			if (!Ext) return;
			if (bGpu)
			{
				Ext->PushRequest_RenderThread({ FrameIdx, Gen, Slot }, nullptr, nullptr, Nv12Readback, Grabbed);
			}
			else
			{
				FTextureRenderTargetResource* Resource = RT->GetRenderTargetResource();
				if (!Resource) return;
				Ext->PushRequest_RenderThread({ FrameIdx, Gen, Slot }, Resource->GetRenderTargetTexture(), Readback, nullptr, Grabbed);
			}
			// Depth (ML) still comes from its own SceneCapture, copied directly.
			EnqueueDepthCopy(RHICmdList, DepthRT, DepthReadback);
		});
		return;
	}

	// Async GPU→CPU DMA on the render thread (returns immediately).
	ENQUEUE_RENDER_COMMAND(CamSimEnqueueReadback)(
		[RT, Readback, DepthRT, DepthReadback, EnqueueDepthCopy](FRHICommandListImmediate& RHICmdList)
	{
		FTextureRenderTargetResource* Resource = RT->GetRenderTargetResource();
		if (!Resource) return;
		FRHITexture* SourceTexture = Resource->GetRenderTargetTexture();

		// Explicit barriers: Metal tracks resources implicitly, Vulkan does not.
		RHICmdList.Transition(FRHITransitionInfo(SourceTexture, ERHIAccess::RTV, ERHIAccess::CopySrc));
		Readback->EnqueueCopy(RHICmdList, SourceTexture);  // non-blocking; the slot's poll checks IsReady()
		RHICmdList.Transition(FRHITransitionInfo(SourceTexture, ERHIAccess::CopySrc, ERHIAccess::RTV));

		EnqueueDepthCopy(RHICmdList, DepthRT, DepthReadback);
	});
}

// -------------------------------------------------------------------------
// Poll — complete the readback (no FlushRenderingCommands)
// -------------------------------------------------------------------------

void UCamSimCaptureComponent::Poll()
{
	checkSlow(IsInGameThread());  // reads game-thread-only state

	// Deliver finished readbacks strictly in capture order. A finished frame
	// waits (in its slot) while the sensor task is busy; the ring backs up
	// and new captures are skipped rather than delivered out of order.
	int32 Slot = INDEX_NONE;
	bool  bFailed = false;
	while (Ring.PeekFinished(Slot, bFailed))
	{
		FSlot& S = Slots[Slot];
		const uint64 FrameIdx = Ring.GetFrameIndex(Slot);
		if (bFailed)
		{
			if (bTrackFrameDrops) FrameDropStats.ReadbackTimeout++;
			UE_LOG(LogCamSim, Warning, TEXT("CamSimReadback frame %llu: readback failed or timed out (lock null, bad format, or never grabbed)"), FrameIdx);
			if (bGpuSensor && ++ConsecutiveGpuFailures >= GpuStallLogThreshold && !bLoggedGpuStall)
			{
				bLoggedGpuStall = true;
				UE_LOG(LogCamSim, Error, TEXT("ACamSimCamera: GPU sensor path: %d consecutive frames were never delivered. ")
					TEXT("The sensor graph replaces UE's tonemapper pass: if that pass is disabled (e.g. ShowFlag.Tonemapper 0 ")
					TEXT("or ShowFlag.PostProcessing 0) the graph never runs. Re-enable it, or restart with render.sensor_path: legacy."),
					ConsecutiveGpuFailures);
			}
			S.Pixels.Reset();
			S.Nv12.Reset();
			S.Depth.Reset();
			Ring.Release(Slot);
			continue;
		}
		if (bSensorBusy) break;
		ConsecutiveGpuFailures = 0;

		if (LatencyTracker) LatencyTracker->Mark(EPipelineStage::ReadbackComplete);

		// ROADMAP 3A reference shots, lossless: the pre-sensor frame on the
		// legacy path, the sensor's NV12 output on the GPU path.
		if (FCamSimSnapshotService* Snap = Subsystem ? Subsystem->GetSnapshotService() : nullptr)
		{
			if (Snap->WantsFrame()) OfferSnapshot(*Snap, S);
		}
		if (FCamSimSnapshotService* SensorSnap = Subsystem ? Subsystem->GetSensorSnapshotService() : nullptr)
		{
			if (SensorSnap->WantsFrame()) OfferSnapshot(*SensorSnap, S);
		}

		bSensorBusy = true;
		SubmitFrameToEncoder(MoveTemp(S.Pixels), MoveTemp(S.Nv12), S.Telemetry, FrameIdx, MoveTemp(S.Depth));
		S.Pixels.Reset();
		S.Nv12.Reset();
		S.Depth.Reset();
		Ring.Release(Slot);
	}

	EnqueuePolls();
}

void UCamSimCaptureComponent::OfferSnapshot(FCamSimSnapshotService& Snap, const FSlot& S) const
{
	const FCamSimConfig& C = Subsystem->GetConfig();
	if (S.Nv12.Num() > 0)
	{
		TArray<FColor> Bgra;
		CamSimNv12::ToBgra(S.Nv12.GetData(), C.CaptureWidth, C.CaptureHeight, Bgra);
		Snap.OfferFrame(Bgra, C.CaptureWidth, C.CaptureHeight);
	}
	else
	{
		Snap.OfferFrame(S.Pixels, C.CaptureWidth, C.CaptureHeight);
	}
}

void UCamSimCaptureComponent::EnqueuePolls()
{
	bool bAny = false;
	for (const int32 Slot : Ring.GetOrder())
	{
		if (Ring.GetState(Slot) == EReadbackSlotState::InFlight)
		{
			EnqueuePoll(Slot);
			bAny = true;
		}
	}
	if (bAny && LatencyTracker) LatencyTracker->Mark(EPipelineStage::ReadbackIssue);
}

void UCamSimCaptureComponent::EnqueuePoll(int32 Slot)
{
	// A non-blocking render command polls the slot's IsReady(); on success it
	// copies the pixels into the slot and marks it Complete (SeqCst). The game
	// thread consumes it on a later tick, so rendering never waits on the DMA.
	// Everything the lambda needs is captured by value.
	const FCamSimConfig& Cfg = Subsystem->GetConfig();
	const int32 ReadyPollsRequired = FMath::Max(1, Cfg.ReadbackReadyPolls);
	FRHIGPUTextureReadback* Readback = ColorReadbackPool.IsValidIndex(Slot) ? ColorReadbackPool[Slot].Get() : nullptr;
	FRHIGPUTextureReadback* DepthReadback = DepthReadbackPool.IsValidIndex(Slot) ? DepthReadbackPool[Slot].Get() : nullptr;
	FRHIGPUBufferReadback*  Nv12Readback = (bGpuSensor && Nv12ReadbackPool.IsValidIndex(Slot)) ? Nv12ReadbackPool[Slot].Get() : nullptr;
	UTextureRenderTarget2D* RT = RenderTargets.IsValidIndex(Slot) ? RenderTargets[Slot].Get() : nullptr;

	const int32  CaptureW       = Cfg.CaptureWidth;
	const int32  CaptureH       = Cfg.CaptureHeight;
	const auto   ReadbackFormat = Cfg.ReadbackFormat;
	const bool   bSwapRB        = Cfg.bSwapRBReadback;
	// The buffer's size, not the live config's (the graph was enabled at GpuSensorSize).
	const uint32 Nv12Bytes      = bGpuSensor ? static_cast<uint32>(CamSimNv12::NumBytes(GpuSensorSize.X, GpuSensorSize.Y)) : 0u;
	FSlot&       S              = Slots[Slot];
	const uint64 FrameIdx       = Ring.GetFrameIndex(Slot);
	const uint32 CaptureGen     = S.Generation.Load(EMemoryOrder::SequentiallyConsistent);
	const bool   bNeedsGrab     = bPrimaryView;
	TAtomic<uint32>* Grabbed    = bPrimaryView ? &GrabbedGeneration[Slot] : nullptr;
	FReadbackRing* RingPtr      = &Ring;

	ENQUEUE_RENDER_COMMAND(CamSimPollReadback)(
		[RingPtr, &S, Slot, Readback, DepthReadback, Nv12Readback, Nv12Bytes, RT, ReadyPollsRequired,
		 CaptureW, CaptureH, ReadbackFormat, bSwapRB, FrameIdx, CaptureGen, bNeedsGrab, Grabbed]
		(FRHICommandListImmediate&)
	{
		// Stale poll: the slot was delivered (and maybe reused) since it was enqueued.
		if (S.Generation.Load(EMemoryOrder::SequentiallyConsistent) != CaptureGen) return;
		// Only while in flight: an earlier poll this tick may already have delivered.
		if (RingPtr->GetState(Slot) != EReadbackSlotState::InFlight) return;

		const uint32 Attempt = S.PollAttempts.Load(EMemoryOrder::Relaxed) + 1;
		S.PollAttempts.Store(Attempt, EMemoryOrder::Relaxed);
		const CamSimReadback::EPollDecision Decision = CamSimReadback::DecidePoll(
			bNeedsGrab, Grabbed ? Grabbed->Load(EMemoryOrder::SequentiallyConsistent) : 0, CaptureGen,
			[Readback, Nv12Readback]() { return Nv12Readback ? Nv12Readback->IsReady() : (Readback && Readback->IsReady()); },
			Attempt, MaxReadbackPolls);
		if (Decision == CamSimReadback::EPollDecision::TimedOut)
		{
			// Never grabbed (viewport not drawn, request dropped) or a stuck fence:
			// fail the slot so the ring keeps moving.
			RingPtr->MarkFailed(Slot);
			return;
		}
		if (Decision == CamSimReadback::EPollDecision::Wait)
		{
			S.ReadyStreak.Store(0, EMemoryOrder::Relaxed);
			return;
		}
		{
			const uint8 Cur = S.ReadyStreak.Load(EMemoryOrder::Relaxed);
			if (Cur < 255) S.ReadyStreak.Store(Cur + 1, EMemoryOrder::Relaxed);
		}
		if (S.ReadyStreak.Load(EMemoryOrder::Relaxed) < ReadyPollsRequired) return;

		if (Nv12Readback)
		{
			// GPU sensor (3B): the sensor graph's NV12 output, tightly packed.
			const void* Raw = Nv12Readback->Lock(Nv12Bytes);
			if (!Raw) { RingPtr->MarkFailed(Slot); return; }  // null: do NOT Unlock
			S.Nv12.SetNumUninitialized(Nv12Bytes);
			FMemory::Memcpy(S.Nv12.GetData(), Raw, Nv12Bytes);
			Nv12Readback->Unlock();
			S.Pixels.Reset();
		}
		else
		{
			int32 RowPitch = 0;
			void* RawData = Readback ? Readback->Lock(RowPitch) : nullptr;
			if (!RawData || !RT)
			{
				if (RawData) Readback->Unlock();
				RingPtr->MarkFailed(Slot);
				return;
			}

			const int32 W = RT->SizeX;
			const int32 H = RT->SizeY;
			const EPixelFormat PixelFormat = RT->GetFormat();
			if (PixelFormat != PF_B8G8R8A8 && PixelFormat != PF_R8G8B8A8)
			{
				Readback->Unlock();
				RingPtr->MarkFailed(Slot);
				return;
			}

			S.Pixels.SetNumUninitialized(W * H);
			CamSimConvertReadbackPixels(RawData, RowPitch, W, H, PixelFormat, ReadbackFormat, bSwapRB, S.Pixels, FrameIdx);
			Readback->Unlock();
		}

		// Opportunistic depth: it may lag color; skip this frame's if so.
		S.Depth.Reset();
		if (DepthReadback && DepthReadback->IsReady())
		{
			{
				const uint8 Cur = S.DepthReadyStreak.Load(EMemoryOrder::Relaxed);
				if (Cur < 255) S.DepthReadyStreak.Store(Cur + 1, EMemoryOrder::Relaxed);
			}
			if (S.DepthReadyStreak.Load(EMemoryOrder::Relaxed) >= ReadyPollsRequired)
			{
				int32 DepthRowPitch = 0;
				if (void* DepthRaw = DepthReadback->Lock(DepthRowPitch))  // null: do NOT Unlock (UB)
				{
					S.Depth.SetNumUninitialized(CaptureW * CaptureH);
					const uint8* Src = static_cast<const uint8*>(DepthRaw);
					float*       Dst = S.Depth.GetData();
					for (int32 Row = 0; Row < CaptureH; ++Row)
					{
						FMemory::Memcpy(Dst + Row * CaptureW, Src + Row * DepthRowPitch, CaptureW * sizeof(float));
					}
					for (float& V : S.Depth) { V /= 100.0f; }  // cm -> m
					DepthReadback->Unlock();
				}
			}
		}
		else
		{
			S.DepthReadyStreak.Store(0, EMemoryOrder::Relaxed);
		}

		// SeqCst: the slot's arrays are visible once the game thread sees Complete.
		RingPtr->MarkComplete(Slot);
	});
}

// -------------------------------------------------------------------------
// SubmitFrameToEncoder — sensor model on a pool thread, then the encoder queue
// -------------------------------------------------------------------------

void UCamSimCaptureComponent::SubmitFrameToEncoder(
	TArray<FColor> PixelData, TArray<uint8> Nv12Data, FCamSimTelemetry Telemetry, uint64 FrameIdx, TArray<float> DepthMetres)
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
		 Pixels = MoveTemp(PixelData), Nv12 = MoveTemp(Nv12Data), Telemetry, FrameIdx, Depth = MoveTemp(DepthMetres)]() mutable
	{
		SCOPE_CYCLE_COUNTER(STAT_CamSimEncode);
		// The CPU sensor model runs only on BGRA (legacy) frames; NV12 frames
		// already went through the GPU sensor graph (3B).
		const bool bNv12 = Nv12.Num() > 0;
		if (LT) LT->Mark(EPipelineStage::SensorStart);
		if (FX && !bNv12)
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
		if (bNv12)
		{
			Frame.Frame.Format = ESensorPixelFormat::NV12;
			Frame.Frame.Nv12   = MoveTemp(Nv12);
		}
		else
		{
			Frame.Frame.Format = ESensorPixelFormat::BGRA8;
			Frame.Frame.Bgra   = MoveTemp(Pixels);
		}
		Frame.Telemetry    = Telemetry;
		Frame.FrameIndex   = FrameIdx;
		EncThread->Enqueue(MoveTemp(Frame));  // non-blocking

		bSensorBusy = false;  // the next capture can start; encode runs independently
	});
}
