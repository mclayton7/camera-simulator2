// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimCaptureComponent.h"
#include "Health/CamSimSnapshotService.h"
#include "Camera/CamSimFrameGrabExtension.h"
#include "Camera/CamSimRenderPath.h"
#include "Engine/GameViewportClient.h"
#include "SceneViewExtension.h"
#include "CamSimTest.h"
#include "Config/CamSimConfig.h"
#include "Diagnostics/PipelineLatencyTracker.h"
#include "Encoder/EncoderThread.h"
#include "Encoder/IFrameSink.h"
#include "Encoder/Nv12.h"
#include "Entity/CamSimEntityManager.h"
#include "GroundTruth/FEntityProjection.h"
#include "GroundTruth/FGroundTruthCollector.h"
#include "GroundTruth/StillWaterPlane.h"
#include "Geospatial/CamSimGeospatialProvider.h"
#include "Ocean/OceanSurface.h"
#include "Subsystem/CamSimSubsystem.h"
#include "Time/SimClock.h"
#include "Sensor/SensorOptics.h"

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

	Sensor->FOVAngle = Cfg.HFovDeg;

	// ROADMAP 3B: the GPU sensor graph is the only sensor path. The subsystem
	// decides once whether it can run (and logs why not); without it no frames
	// are produced.
	bSensorGraph = Subsystem->IsSensorGraphAvailable();
	if (bSensorGraph)
	{
		GpuSensorSize = FIntPoint(Cfg.CaptureWidth, Cfg.CaptureHeight);
		Nv12ReadbackPool.Reset();
		for (int32 Idx = 0; Idx < FReadbackRing::NumSlots; ++Idx)
		{
			Nv12ReadbackPool.Add(MakeUnique<FRHIGPUBufferReadback>(*FString::Printf(TEXT("CamSimNv12Readback_%d"), Idx)));
		}
	}
	else
	{
		UE_LOG(LogCamSim, Error, TEXT("ACamSimCamera: sensor graph unavailable — no frames will be produced (see the subsystem's startup error)"));
	}

	// Ground truth (ROADMAP 2.7): per-slot instance-ID readbacks, so annotated
	// frames measure boxes/masks from the render. InstanceIdCS is checked apart
	// from the sensor graph: a missing ID shader only costs the measured boxes.
	IdReadbackPool.Reset();
	IdIntervalFrames = FMath::Max(1, Cfg.MLTraining.AnnotationIntervalFrames);
	if (bSensorGraph && Subsystem->IsGroundTruthMaskAvailable())   // decided once by the subsystem (logs why not)
	{
		for (int32 Idx = 0; Idx < FReadbackRing::NumSlots; ++Idx)
		{
			IdReadbackPool.Add(MakeUnique<FRHIGPUBufferReadback>(*FString::Printf(TEXT("CamSimInstanceIdReadback_%d"), Idx)));
		}
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: ground truth — instance-ID readback enabled (every %d frame(s))"), IdIntervalFrames);
	}

	for (TAtomic<uint32>& Gen : GrabbedGeneration) { Gen.Store(0); }
	for (TAtomic<uint32>& Gen : IdGrabbedGeneration) { Gen.Store(0); }
	if (bSensorGraph && !EnsureGrabExtension())
	{
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: primary view — game viewport not created yet; grabbing starts when it is"));
	}

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
	GrabExtension = FSceneViewExtensions::NewExtension<FCamSimFrameGrabExtension>(Viewport, GpuSensorSize, &StatsMailbox);
	UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: primary view — the GPU sensor graph replaces the game viewport's tonemapper"));
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
	Nv12ReadbackPool.Reset();
	IdReadbackPool.Reset();
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
	// The primary view's game viewport show flags get the same values as the capture's.
	FEngineShowFlags* ViewFlags = (GEngine && GEngine->GameViewport)
		? &GEngine->GameViewport->EngineShowFlags : nullptr;
	// The game viewport's canvas (map warnings, renderer notices such as
	// Lumen's exposure-range warning, debug messages) would be burned into
	// the grabbed frame. Screenshots and movie dumps suppress it the same way.
	GAreScreenMessagesEnabled = false;

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
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: optical realism enabled (blur=%d bloom=%d DoF=%d flare=%d)"),
			O.bMotionBlur, O.bBloom, O.bDepthOfField, O.bLensFlare);
	}

	// ROADMAP 3B: the sensor owns exposure. UE runs manual so its eye adaptation
	// never fights the sensor AE; UpdateSensorParams sets the bias each tick only
	// to keep scene colour in fp16 range (the graph divides PreExposure back out).
	PP.bOverride_AutoExposureMethod = true;
	PP.AutoExposureMethod = AEM_Manual;
	PP.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	PP.AutoExposureApplyPhysicalCameraExposure = false;
	PP.bOverride_AutoExposureBias = true;
	PP.AutoExposureBias = 0.0f;

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
		// Primary view: TSR (4) with full view history.
		SetCVarI(TEXT("r.AntiAliasingMethod"), 4);

		UE_LOG(LogCamSim, Log,
			TEXT("ACamSimCamera: RenderingQuality — shadows=%d contactShadow=%d AO=%.2f "
			     "RTRefl=%d shadowDist=%.1f VSMBias=%d TSR%%=%d AA=%s"),
			(int)RQ.bEntityShadows, (int)RQ.bContactShadows, RQ.AOIntensity,
			(int)RQ.bRayTracedReflections, RQ.ShadowDistanceScale,
			RQ.VSMResolutionBias, RQ.TSRScreenPercentage, TEXT("TSR"));
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
}

void UCamSimCaptureComponent::UpdateSensorParams(ESensorMode Mode, uint8 Polarity, bool bCameraCut, float LiveHFovDeg, const FCamSimConfig& Cfg)
{
	if (!bSensorGraph || !GrabExtension) return;

	const double NowSimSec = static_cast<double>(FSimClock::Get().NowMicros()) * 1e-6;
	const double Dt = LastSensorUpdateSimSec < 0.0 ? 0.0 : NowSimSec - LastSensorUpdateSimSec;
	LastSensorUpdateSimSec = NowSimSec;

	FSensorHistogram Hist;
	const bool bHasHist = StatsMailbox.TakeLatest(Hist);

	FSensorControllerInput In;
	In.Mode         = static_cast<ESensorGraphMode>(FMath::Clamp(static_cast<int32>(Mode), 0, 1));
	In.bBlackHot    = Polarity != 0;
	In.bCameraCut   = bCameraCut;
	In.DeltaSimSec  = Dt;
	In.NewHistogram = bHasHist ? &Hist : nullptr;
	In.Serial       = ++ParamsSerial;
	// One detector integration per rendered frame. The controller copies the
	// mode's seed and detector config into the params (DarkE = DarkCurrentEs /
	// FrameRateHz, AdcMax = 2^bits - 1, FrameIndex = Serial).
	In.FrameRateHz  = Cfg.Performance.RenderFrameRateHz > 0.0f ? Cfg.Performance.RenderFrameRateHz : 30.0f;
	const FSensorModeConfig* ModeCfg = Cfg.SensorModeConfigs.Find(Mode);
	static const FSensorModeConfig DefaultModeCfg;
	const FSensorModeConfig& MC = ModeCfg ? *ModeCfg : DefaultModeCfg;
	const uint32 StaleBefore = SensorController.GetStaleEpisodes();
	FSensorFrameParams Params = SensorController.Update(In, MC);
	if (SensorController.GetStaleEpisodes() != StaleBefore && bTrackFrameDrops) FrameDropStats.SensorStatsStale++;

	// Lens model (ROADMAP 3B.2): focal length from this frame's FOV, so zoom changes the
	// distortion/vignetting footprint. Recomputed only when the lens or the FOV changes.
	if (!OpticsCache.bValid || OpticsCache.Mode != Mode || OpticsCache.HFovDeg != LiveHFovDeg || !(OpticsCache.Lens == MC.Optics))
	{
		OpticsCache.bValid     = true;
		OpticsCache.Mode       = Mode;
		OpticsCache.HFovDeg    = LiveHFovDeg;
		OpticsCache.Lens       = MC.Optics;
		OpticsCache.bConverges = CamSimOptics::SetOptics(OpticsCache.Params, MC.Optics, Cfg.CaptureWidth, Cfg.CaptureHeight, LiveHFovDeg);
		if (!OpticsCache.bConverges && !bLoggedDistortionFallback)
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("Sensor optics: distortion k1=%.3f k2=%.3f does not converge at the live HFOV %.2f deg; "
				     "rendering without distortion until the FOV narrows (logged once)"),
				MC.Optics.K1, MC.Optics.K2, LiveHFovDeg);
			bLoggedDistortionFallback = true;
		}
		else if (OpticsCache.bConverges)
		{
			bLoggedDistortionFallback = false;
		}
	}
	const FSensorFrameParams& O = OpticsCache.Params;
	Params.FocalPx            = O.FocalPx;
	Params.K1                 = O.K1;
	Params.K2                 = O.K2;
	Params.VignettingExponent = O.VignettingExponent;
	Params.PsfSigmaPx         = O.PsfSigmaPx;
	FMemory::Memcpy(Params.PsfTaps, O.PsfTaps, sizeof(Params.PsfTaps));
	Params.NumPsfTaps         = O.NumPsfTaps;

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
// Capture — request the sensor graph's NV12 output for the next frame
// -------------------------------------------------------------------------
// Poll() completes it on later ticks. Frame N+1 renders on the game thread
// while the render thread finishes frame N's DMA and the background task runs.

bool UCamSimCaptureComponent::IsReadyForCapture() const
{
	return Ring.CanAcquire();
}

void UCamSimCaptureComponent::NoteCaptureSkipped()
{
	// Every slot is in flight or waiting for the sensor task: downstream is behind.
	if (bTrackFrameDrops) FrameDropStats.EncoderBusy++;
}

TArray<FEntityAnnotationData> UCamSimCaptureComponent::BuildGroundTruthSnapshot() const
{
	FGroundTruthCollector* Collector = Subsystem->GetGroundTruthCollector();
	FCamSimEntityManager*  EntityMgr = Subsystem->GetEntityManager();
	if (!Collector || !Collector->IsEnabled() || !EntityMgr) return TArray<FEntityAnnotationData>();

	const FCamSimConfig& Cfg = Subsystem->GetConfig();
	FViewProjectionData ViewProj;
	ViewProj.ViewProjectionMatrix = FEntityProjection::BuildViewProjectionMatrix(
		Sensor->GetComponentLocation(), Sensor->GetComponentRotation(), Sensor->FOVAngle,
		Cfg.CaptureWidth, Cfg.CaptureHeight);
	ViewProj.ImageWidth  = Cfg.CaptureWidth;
	ViewProj.ImageHeight = Cfg.CaptureHeight;
	if (OpticsCache.bValid)
	{
		// The same optics this frame's sensor graph resamples with.
		ViewProj.FocalPx = OpticsCache.Params.FocalPx;
		ViewProj.K1      = OpticsCache.Params.K1;
		ViewProj.K2      = OpticsCache.Params.K2;
	}

	// Cheap cone-cull hint, ~30% wider than the HFOV so entities barely
	// outside still get projected (the AABB projection clips correctly).
	ViewProj.CameraLocation = Sensor->GetComponentLocation();
	ViewProj.CameraForward  = Sensor->GetForwardVector();
	ViewProj.CullConeHalfAngleCos = FMath::Cos(FMath::DegreesToRadians(FMath::Min(90.0f, Sensor->FOVAngle * 0.65f)));

	return EntityMgr->GetEntitySnapshot(ViewProj);
}

FStillWaterPlane UCamSimCaptureComponent::ComputeWaterPlane(const FCamSimTelemetry& T) const
{
	const FOceanSurface* Ocean = Subsystem ? Subsystem->GetOceanSurface() : nullptr;   // null: ocean off
	const FCamSimGeospatialProvider* Geo = Subsystem ? Subsystem->GetGeospatialProvider() : nullptr;
	UWorld* World = GetWorld();
	if (!Ocean || !Geo || !World) return FStillWaterPlane();
	// As FOceanManager::Tick (live: ocean.max_radius_km is hot-reloadable; a bad value means the default).
	double MaxRadiusKm = Subsystem->GetConfig().Ocean.MaxRadiusKm;
	if (!FMath::IsFinite(MaxRadiusKm) || MaxRadiusKm <= 0.0) MaxRadiusKm = FCamSimConfig::FOceanConfig().MaxRadiusKm;
	return CamSimGroundTruth::ComputeStillWaterPlane(*Ocean, T.Latitude, T.Longitude, T.Altitude,
		T.FrameCenterLat, T.FrameCenterLon, T.FrameCenterLat != 0.0 || T.FrameCenterLon != 0.0, MaxRadiusKm,
		[Geo, World](double Lat, double Lon, double AltM, FVector& Out) { return Geo->GeoToWorld(World, Lat, Lon, AltM, Out); });
}

void UCamSimCaptureComponent::Capture(const FCamSimTelemetry& Telemetry)
{
	if (!Sensor || !bSensorGraph) return;       // no sensor graph: no frames (logged at startup)
	if (!EnsureGrabExtension()) return;         // nothing to grab yet
	if (!Ring.CanAcquire()) { NoteCaptureSkipped(); return; }

	const uint64 FrameIdx = FrameIndex;
	const int32  Slot     = Ring.Acquire(FrameIdx);
	if (!Nv12ReadbackPool.IsValidIndex(Slot) || !Nv12ReadbackPool[Slot])
	{
		Ring.MarkFailed(Slot);  // released by Poll() like any failed readback
		return;
	}
	++FrameIndex;

	FSlot& S = Slots[Slot];
	// Entity annotation snapshot for this exact view (Phase 17D), rides in the
	// slot like Telemetry — up to three frames are in flight in the readback
	// ring, so this must not be shared collector state (ROADMAP: ground truth
	// per-frame snapshots).
	// Only frames the collector annotates (same FrameIdx and interval) get a snapshot and IDs.
	const bool bAnnotated = (FrameIdx % static_cast<uint64>(IdIntervalFrames)) == 0;
	S.Entities = (Subsystem && bAnnotated) ? BuildGroundTruthSnapshot() : TArray<FEntityAnnotationData>();
	const uint32 Gen = ++NextGeneration;
	S.Telemetry = Telemetry;
	S.bWantIds = bAnnotated && IdReadbackPool.IsValidIndex(Slot) && IdReadbackPool[Slot];
	S.InstanceIds.Reset();
	S.ReadyStreak     .Store(0, EMemoryOrder::Relaxed);
	S.DepthReadyStreak.Store(0, EMemoryOrder::Relaxed);
	S.PollAttempts    .Store(0, EMemoryOrder::Relaxed);
	// SeqCst: the resets above are visible to any poll that sees the new generation.
	S.Generation.Store(Gen, EMemoryOrder::SequentiallyConsistent);

	// Depth alongside the sensor frame (Phase 17A), in the same slot.
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

	// The game viewport renders after this tick; the sensor graph runs in place
	// of its tonemapper and the extension copies its NV12 output into this
	// slot's readback. Enqueued now, so it's the newest request when this
	// frame's scene render runs.
	TSharedPtr<FCamSimFrameGrabExtension, ESPMode::ThreadSafe> Ext = GrabExtension;
	TAtomic<uint32>* Grabbed = &GrabbedGeneration[Slot];
	TAtomic<uint32>* IdGrabbed = &IdGrabbedGeneration[Slot];
	FRHIGPUBufferReadback* Nv12Readback = Nv12ReadbackPool[Slot].Get();
	FRHIGPUBufferReadback* IdReadback = S.bWantIds ? IdReadbackPool[Slot].Get() : nullptr;
	FFrameGrabRequest Req;
	Req.FrameIndex   = FrameIdx;
	Req.Generation   = Gen;
	Req.TargetIndex  = Slot;
	Req.bInstanceIds = S.bWantIds;
	// Still-water plane for the submerged-hull cut, at this frame's camera (game thread, doubles).
	if (S.bWantIds) Req.WaterPlane = ComputeWaterPlane(Telemetry);
	ENQUEUE_RENDER_COMMAND(CamSimRequestGrab)(
		[Ext, Nv12Readback, IdReadback, Req, Grabbed, IdGrabbed, DepthRT, DepthReadback, EnqueueDepthCopy]
		(FRHICommandListImmediate& RHICmdList)
	{
		if (!Ext) return;
		Ext->PushRequest_RenderThread(Req, Nv12Readback, IdReadback, Grabbed, IdGrabbed);
		// Depth (ML) comes from its own SceneCapture, copied directly.
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
	// waits (in its slot) while the background task is busy; the ring backs up
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
			if (++ConsecutiveGpuFailures >= GpuStallLogThreshold && !bLoggedGpuStall)
			{
				bLoggedGpuStall = true;
				UE_LOG(LogCamSim, Error, TEXT("ACamSimCamera: GPU sensor graph: %d consecutive frames were never delivered. ")
					TEXT("The sensor graph replaces UE's tonemapper pass: if that pass is disabled (e.g. ShowFlag.Tonemapper 0 ")
					TEXT("or ShowFlag.PostProcessing 0) the graph never runs. Re-enable it."),
					ConsecutiveGpuFailures);
			}
			S.Nv12.Reset();
			S.Depth.Reset();
			S.Entities.Reset();
			S.InstanceIds.Reset();
			Ring.Release(Slot);
			continue;
		}
		if (bSensorBusy) break;
		ConsecutiveGpuFailures = 0;

		if (LatencyTracker) LatencyTracker->Mark(EPipelineStage::ReadbackComplete);

		// ROADMAP 3A reference shots, lossless: the sensor's NV12 output.
		if (FCamSimSnapshotService* Snap = Subsystem ? Subsystem->GetSnapshotService() : nullptr)
		{
			if (Snap->WantsFrame()) OfferSnapshot(*Snap, S);
		}
		if (FCamSimSnapshotService* SensorSnap = Subsystem ? Subsystem->GetSensorSnapshotService() : nullptr)
		{
			if (SensorSnap->WantsFrame()) OfferSnapshot(*SensorSnap, S);
		}

		bSensorBusy = true;
		SubmitFrameToEncoder(MoveTemp(S.Nv12), S.Telemetry, FrameIdx, MoveTemp(S.Depth), MoveTemp(S.Entities),
			MoveTemp(S.InstanceIds));
		S.Nv12.Reset();
		S.Depth.Reset();
		S.Entities.Reset();
		S.InstanceIds.Reset();
		Ring.Release(Slot);
	}

	EnqueuePolls();
}

void UCamSimCaptureComponent::OfferSnapshot(FCamSimSnapshotService& Snap, const FSlot& S) const
{
	// The size the sensor graph was set up with (never the live, hot-reloadable config).
	if (S.Nv12.Num() != CamSimNv12::NumBytes(GpuSensorSize.X, GpuSensorSize.Y)) return;
	TArray<FColor> Bgra;
	CamSimNv12::ToBgra(S.Nv12.GetData(), GpuSensorSize.X, GpuSensorSize.Y, Bgra);
	Snap.OfferFrame(Bgra, GpuSensorSize.X, GpuSensorSize.Y);
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
	// copies the NV12 frame into the slot and marks it Complete (SeqCst). The game
	// thread consumes it on a later tick, so rendering never waits on the DMA.
	// Everything the lambda needs is captured by value.
	const FCamSimConfig& Cfg = Subsystem->GetConfig();
	const int32 ReadyPollsRequired = FMath::Max(1, Cfg.ReadbackReadyPolls);
	FRHIGPUTextureReadback* DepthReadback = DepthReadbackPool.IsValidIndex(Slot) ? DepthReadbackPool[Slot].Get() : nullptr;
	FRHIGPUBufferReadback*  Nv12Readback = Nv12ReadbackPool.IsValidIndex(Slot) ? Nv12ReadbackPool[Slot].Get() : nullptr;

	const int32  CaptureW       = Cfg.CaptureWidth;
	const int32  CaptureH       = Cfg.CaptureHeight;
	// The buffer's size, not the live config's (the graph was enabled at GpuSensorSize).
	const uint32 Nv12Bytes      = static_cast<uint32>(CamSimNv12::NumBytes(GpuSensorSize.X, GpuSensorSize.Y));
	FSlot&       S              = Slots[Slot];
	const uint64 FrameIdx       = Ring.GetFrameIndex(Slot);
	const uint32 CaptureGen     = S.Generation.Load(EMemoryOrder::SequentiallyConsistent);
	TAtomic<uint32>* Grabbed    = &GrabbedGeneration[Slot];
	FReadbackRing* RingPtr      = &Ring;
	// Ground truth: this slot's instance-ID copy (16 bits per output pixel).
	const bool   bWantIds       = S.bWantIds;
	FRHIGPUBufferReadback* IdReadback = (bWantIds && IdReadbackPool.IsValidIndex(Slot)) ? IdReadbackPool[Slot].Get() : nullptr;
	TAtomic<uint32>* IdGrabbed  = &IdGrabbedGeneration[Slot];
	const uint32 IdBytes        = static_cast<uint32>(GpuSensorSize.X * GpuSensorSize.Y * 2);

	ENQUEUE_RENDER_COMMAND(CamSimPollReadback)(
		[RingPtr, &S, Slot, DepthReadback, Nv12Readback, Nv12Bytes, ReadyPollsRequired,
		 CaptureW, CaptureH, CaptureGen, Grabbed, IdReadback, IdGrabbed, IdBytes]
		(FRHICommandListImmediate&)
	{
		// Stale poll: the slot was delivered (and maybe reused) since it was enqueued.
		if (S.Generation.Load(EMemoryOrder::SequentiallyConsistent) != CaptureGen) return;
		// Only while in flight: an earlier poll this tick may already have delivered.
		if (RingPtr->GetState(Slot) != EReadbackSlotState::InFlight) return;

		const uint32 Attempt = S.PollAttempts.Load(EMemoryOrder::Relaxed) + 1;
		S.PollAttempts.Store(Attempt, EMemoryOrder::Relaxed);
		const CamSimReadback::EPollDecision Decision = CamSimReadback::DecidePoll(
			Grabbed->Load(EMemoryOrder::SequentiallyConsistent), CaptureGen,
			// The ID copy (issued before the NV12 one) is waited for only if it was
			// really issued for this capture; a pass that couldn't run never blocks.
			[Nv12Readback, IdReadback, IdGrabbed, CaptureGen]()
			{
				if (!Nv12Readback || !Nv12Readback->IsReady()) return false;
				const bool bWaitIds = CamSimReadback::ShouldWaitForIds(IdReadback != nullptr,
					IdGrabbed->Load(EMemoryOrder::SequentiallyConsistent), CaptureGen);
				return !bWaitIds || IdReadback->IsReady();
			},
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

		{
			// The sensor graph's NV12 output (3B), tightly packed.
			const void* Raw = Nv12Readback->Lock(Nv12Bytes);
			if (!Raw) { RingPtr->MarkFailed(Slot); return; }  // null: do NOT Unlock
			S.Nv12.SetNumUninitialized(Nv12Bytes);
			FMemory::Memcpy(S.Nv12.GetData(), Raw, Nv12Bytes);
			Nv12Readback->Unlock();
		}

		// Instance IDs: a failed lock leaves the slot without them (the collector
		// falls back to projected boxes); it never fails the frame.
		S.InstanceIds.Reset();
		if (CamSimReadback::ShouldWaitForIds(IdReadback != nullptr, IdGrabbed->Load(EMemoryOrder::SequentiallyConsistent), CaptureGen))
		{
			if (const void* Raw = IdReadback->Lock(IdBytes))  // null: do NOT Unlock
			{
				S.InstanceIds.SetNumUninitialized(IdBytes / sizeof(uint32));
				FMemory::Memcpy(S.InstanceIds.GetData(), Raw, IdBytes);
				IdReadback->Unlock();
			}
		}

		// Opportunistic depth: it may lag the sensor frame; skip this frame's if so.
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
// SubmitFrameToEncoder — ground truth on a pool thread, then the encoder queue
// -------------------------------------------------------------------------

void UCamSimCaptureComponent::SubmitFrameToEncoder(
	TArray<uint8> Nv12Data, FCamSimTelemetry Telemetry, uint64 FrameIdx, TArray<float> DepthMetres,
	TArray<FEntityAnnotationData> Entities, TArray<uint32> InstanceIds)
{
	if (!EncoderThread)
	{
		bSensorBusy = false;
		return;
	}

	FGroundTruthCollector*   Collector = Subsystem ? Subsystem->GetGroundTruthCollector() : nullptr;
	const int32              CaptureW  = Subsystem ? Subsystem->GetConfig().CaptureWidth  : 0;
	const int32              CaptureH  = Subsystem ? Subsystem->GetConfig().CaptureHeight : 0;
	FEncoderThread*          EncThread = EncoderThread.Get();  // outlives the task
	FPipelineLatencyTracker* LT        = LatencyTracker;       // outlives the task
	const FIntPoint          IdSize    = GpuSensorSize;        // the ID buffer's size (fixed at Initialize)

	// The sensor model already ran on the GPU (3B). The ML ground-truth writers
	// (file I/O) stay off the game thread; bSensorBusy is cleared after them,
	// not after encode.
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
		[this, EncThread, Collector, CaptureW, CaptureH, LT,
		 Nv12 = MoveTemp(Nv12Data), Telemetry, FrameIdx, Depth = MoveTemp(DepthMetres),
		 Entities = MoveTemp(Entities), IdSize, InstanceIds = MoveTemp(InstanceIds)]() mutable
	{
		SCOPE_CYCLE_COUNTER(STAT_CamSimEncode);
		// SensorStart/End bracket this background stage (latency tracking, 28G).
		if (LT) LT->Mark(EPipelineStage::SensorStart);

		// ML ground truth annotation and depth (Phase 17)
		if (Collector)
		{
			// The frame's instance-ID image when it was read back (measured boxes and
			// masks); otherwise the collector writes projected boxes.
			FInstanceIdImage Ids{ IdSize.X, IdSize.Y, MoveTemp(InstanceIds) };
			Collector->WriteAnnotationFrame(MoveTemp(Entities), Ids.Words.Num() ? &Ids : nullptr, Telemetry, FrameIdx);
			if (Depth.Num() > 0)
			{
				Collector->WriteDepthFrame(Depth, CaptureW, CaptureH, FrameIdx);
			}
		}
		if (LT) LT->Mark(EPipelineStage::SensorEnd);

		FProcessedFrame Frame;
		Frame.Frame.Nv12   = MoveTemp(Nv12);
		Frame.Telemetry    = Telemetry;
		Frame.FrameIndex   = FrameIdx;
		EncThread->Enqueue(MoveTemp(Frame));  // non-blocking

		bSensorBusy = false;  // the next capture can start; encode runs independently
	});
}
