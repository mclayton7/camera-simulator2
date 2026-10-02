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
#include "GroundTruth/SeaSurfacePlane.h"
#include "Geospatial/CamSimGeospatialProvider.h"
#include "Ocean/OceanSurface.h"
#include "Subsystem/CamSimSubsystem.h"
#include "Time/SimClock.h"
#include "Sensor/SensorOptics.h"
#include "Thermal/ThermalFrameSources.h"
#include "CesiumGeoreference.h"

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

// Diagnostics (ROADMAP 4A): log the thermal builder's class temperatures, sky, K_lum, signal scale and AE gain once a second.
static TAutoConsoleVariable<int32> CVarCamSimThermalLog(TEXT("camsim.Thermal.Log"), 0,
	TEXT("1: log the thermal frame parameters once a second while IR runs thermal"), ECVF_Default);

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
	// ML depth map (Phase 17A): InstanceIdCS's depth output from the primary view's
	// scene depth (lens-distorted and FOV-matched with the image), not a second render.
	DepthReadbackPool.Reset();
	if (bSensorGraph && Subsystem->IsGroundTruthDepthAvailable())
	{
		for (int32 Idx = 0; Idx < FReadbackRing::NumSlots; ++Idx)
		{
			DepthReadbackPool.Add(MakeUnique<FRHIGPUBufferReadback>(*FString::Printf(TEXT("CamSimDepthReadback_%d"), Idx)));
		}
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: ground truth — depth map from the primary view (%dx%d, every %d frame(s))"),
			GpuSensorSize.X, GpuSensorSize.Y, IdIntervalFrames);
	}

	for (TAtomic<uint32>& Gen : GrabbedGeneration) { Gen.Store(0); }
	for (TAtomic<uint32>& Gen : IdGrabbedGeneration) { Gen.Store(0); }
	for (TAtomic<uint32>& Gen : DepthGrabbedGeneration) { Gen.Store(0); }
	if (bSensorGraph && !EnsureGrabExtension())
	{
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: primary view — game viewport not created yet; grabbing starts when it is"));
	}

	ApplyRenderSettings(Cfg);

	// Persistent encoder thread (decouples the sensor model from encoding).
	if (IFrameSink* Enc = Subsystem->GetVideoEncoder())
	{
		// Reset(new …): MakeUnique's default deleter can't convert to ours.
		EncoderThread.Reset(new FEncoderThread(Enc, Cfg.FrameRate));
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
	// Let the frame's background task (it enqueues into the encoder thread) and
	// any depth-map writes (they use the collector) finish before both go away.
	const double WaitUntil = FPlatformTime::Seconds() + 5.0;
	while ((bSensorBusy || DepthWritesInFlight.Load(EMemoryOrder::SequentiallyConsistent) > 0)
		&& FPlatformTime::Seconds() < WaitUntil)
	{
		FPlatformProcess::Sleep(0.005f);
	}

	// Stop the encoder thread before the subsystem closes the encoder.
	if (EncoderThread)
	{
		EncoderThread->Stop();
		EncoderThread.Reset();
	}

	if (GrabExtension) { GrabExtension->Detach_GameThread(); }
	ThermalTsrAlpha.Restore(FThermalTsrAlpha::FindCVar());

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

	// Motion blur off on both the capture and the primary view: UE's default
	// (on, amount 0.5) smears every gimbal slew.
	CamSimRender::DisableMotionBlur(PP, Sensor->ShowFlags);
	if (ViewFlags) ViewFlags->SetMotionBlur(false);

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
			     "shadowDist=%.1f VSMBias=%d TSR%%=%d AA=%s"),
			(int)RQ.bEntityShadows, (int)RQ.bContactShadows, RQ.AOIntensity,
			RQ.ShadowDistanceScale,
			RQ.VSMResolutionBias, RQ.TSRScreenPercentage, TEXT("TSR"));
	}

	// frame_rate: the engine renders (and the sensor integrates) one frame per output frame.
	const float TargetRenderFps = FMath::Clamp(Cfg.FrameRate, 1.0f, 120.0f);
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

	UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: Performance — FPS=%.0f texturePoolMB=%d"),
		TargetRenderFps, Cfg.Performance.TexturePoolBudgetMB);
}

void UCamSimCaptureComponent::UpdateSensorParams(ESensorMode Mode, uint8 Polarity, bool bCameraCut, float LiveHFovDeg, const FCamSimConfig& Cfg)
{
	if (!bSensorGraph || !GrabExtension) return;

	const double NowSimSec = static_cast<double>(FSimClock::Get().NowMicros()) * 1e-6;
	const double Dt = LastSensorUpdateSimSec < 0.0 ? 0.0 : NowSimSec - LastSensorUpdateSimSec;
	LastSensorUpdateSimSec = NowSimSec;

	FSensorHistogram Hist;
	const bool bHasHist = StatsMailbox.TakeLatest(Hist);

	const FSensorModeConfig* ModeCfg = Cfg.SensorModeConfigs.Find(Mode);
	static const FSensorModeConfig DefaultModeCfg;
	const FSensorModeConfig& MC = ModeCfg ? *ModeCfg : DefaultModeCfg;

	// ROADMAP 4A: IR renders thermal radiance when ThermalCS is available (startup), thermal.enabled (live) and the render
	// thread hasn't found its inputs missing (then this tick already renders the luminance proxy).
	const bool bThermal = ShouldRunThermal(Mode, CamSimThermalAvailability::IsAvailableThisTick(
		Subsystem && Subsystem->IsThermalAvailable(), Cfg.Thermal.bEnabled, GrabExtension->AreThermalInputsMissing()));
	TSharedPtr<FThermalFrameParams, ESPMode::ThreadSafe> ThermalParams;
	TSharedPtr<const FLandCoverGpuWindow, ESPMode::ThreadSafe> LandCoverGpu;   // ROADMAP 4B: travels with this tick's thermal params
	if (bThermal)
	{
		ThermalBuilder.Configure(Cfg.Thermal, MC.Detector.BandLoUm, MC.Detector.BandHiUm);
		FThermalFrameInputs TIn;
		CamSimThermal::GatherFrameInputs(GetWorld(), *Subsystem, ThermalLat, ThermalLon, ThermalAlt, ThermalUp, TIn);
		// ROADMAP 4B: land cover. The window updates (and builds off-thread) only while thermal IR runs; its East/North axes are
		// recomputed every tick at the window centre (Cesium origin shifts rotate the UE axes) and checked against the Up there
		// (orthonormal, East-South-Up handedness): a bad frame renders land cover off.
		if (Cfg.Thermal.LandCover.bEnabled)
		{
			FLandCoverWindow::FSettings LS;
			LS.Dir              = Cfg.Thermal.LandCover.Dir;
			LS.Texels           = Cfg.Thermal.LandCover.WindowTexels;
			LS.RecentreFraction = Cfg.Thermal.LandCover.RecentreFraction;
			LandCover.Configure(LS);
			LandCover.Update(ThermalLat, ThermalLon);
			for (const FString& W : LandCover.TakeWarnings()) { UE_LOG(LogCamSim, Warning, TEXT("Thermal land cover: %s"), *W); }
			const TSharedPtr<const FLandCoverWindowData, ESPMode::ThreadSafe> Window = LandCover.GetCurrent();
			if (!ThermalGeoreference.IsValid() && !bLoggedLandCoverNoGeoreference)
			{
				UE_LOG(LogCamSim, Warning, TEXT("Thermal land cover: no Cesium georeference, so the window has no East/North axes; "
					"land cover off (logged once)"));
				bLoggedLandCoverNoGeoreference = true;
			}
			if (Window.IsValid() && ThermalGeoreference.IsValid())
			{
				FVector East = FVector::ZeroVector, North = FVector::ZeroVector, CentreUp = FVector::ZeroVector;
				CamSimThermal::LandCoverAxesWorld(*ThermalGeoreference.Get(), Window->Spec.CentreLatDeg, Window->Spec.CentreLonDeg,
					East, North, &CentreUp);
				// Checked against Up at the window centre (same ESU frame), not the camera's: after a long teleport the camera can be
				// far from the not-yet-recentred window, and its up would fail the horizontality check and spend the one warning.
				if (!CamSimThermal::AreLandCoverAxesValid(East, North, CentreUp))
				{
					if (!bLoggedLandCoverAxes)
					{
						UE_LOG(LogCamSim, Warning, TEXT("Thermal land cover: window #%u axes failed the orthonormal/handedness check "
							"(E=%s N=%s Up=%s); land cover off for those frames (logged once)"),
							Window->Id, *East.ToString(), *North.ToString(), *CentreUp.ToString());
						bLoggedLandCoverAxes = true;
					}
				}
				else if (CamSimThermal::SetLandCover(TIn, Window.Get(), East, North))
				{
					LandCoverGpu = LandCover.GetCurrentGpu();
				}
			}
		}
		ThermalParams = MakeShared<FThermalFrameParams, ESPMode::ThreadSafe>();
		TArray<FString> Warnings;
		ThermalBuilder.Build(TIn, *ThermalParams, &Warnings);
		LandCoverWindowIdLastTick = ThermalParams->bLandCover != 0u ? ThermalParams->LandCoverWindowId : 0u;
		for (const FString& W : Warnings) { UE_LOG(LogCamSim, Warning, TEXT("Thermal: %s"), *W); }
		if (CVarCamSimThermalLog.GetValueOnGameThread() > 0 && (ParamsSerial % 30u) == 0u)
		{
			const FThermalFrameParams& T = *ThermalParams;
			FString Classes;
			for (uint32 C = 0; C < FMath::Min<uint32>(T.NumClasses, 8u); ++C) { Classes += FString::Printf(TEXT(" %.1f"), T.ClassTempK[C]); }
			UE_LOG(LogCamSim, Log, TEXT("Thermal: classes[K]%s terrain=%u water=%u Tair=%.1f K cloud=%.2f epsZ=%.3f KLum=%.1f "
				"KFast=%.2f EClamp=%.0f signalScale=%.4g gainEv=%.2f sunLux=%.0f entities=%d landCover=%u warp=%.1f/%.0f m anchor=(%.0f, %.0f)"),
				*Classes, T.TerrainClass, T.WaterClass, T.TairK, T.Cloud, T.SkyEpsZ, T.KLum, T.KFastScale, T.EClampWm2,
				ThermalBuilder.GetSignalScale(), SensorController.GetGainEv(), TIn.SunIlluminanceLux, TIn.Entities.Num(),
				T.bLandCover != 0u ? T.LandCoverWindowId : 0u, T.LandCoverWarpAmpM, T.LandCoverWarpCellM, T.LandCoverAnchorM.X, T.LandCoverAnchorM.Y);
		}
	}
	bThermalActiveLastTick = bThermal;
	if (!bThermal) LandCoverWindowIdLastTick = 0u;

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
	In.FrameRateHz  = Cfg.FrameRate > 0.0f ? Cfg.FrameRate : 30.0f;
	In.bRadianceInput = bThermal;   // thermal AE slot + capped AGC (ROADMAP 4A)
	const uint32 StaleBefore = SensorController.GetStaleEpisodes();
	FSensorFrameParams Params = SensorController.Update(In, MC);
	if (SensorController.GetStaleEpisodes() != StaleBefore) FrameDropStats.SensorStatsStale++;
	if (bThermal)
	{
		// The graph's input is the TSR-resolved RGBA16F scene colour holding (L, L, L, 1); the detector signal is L / B(300 K).
		Params.SignalWeights = FVector3f(1.0f, 0.0f, 0.0f);
		Params.InputScale    = ThermalBuilder.GetSignalScale();
	}

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

	// Keep UE's pre-exposure near the sensor gain so scene colour stays in fp16 range. Thermal: the sensor gain acts on
	// radiance, not on scene colour, so UE's exposure is fixed (ThermalCS only reads colour for the solar term and
	// divides PreExposure back out).
	Sensor->PostProcessSettings.AutoExposureBias = bThermal ? ThermalUeExposureEv : SensorController.GetGainEv() + UeExposureOffsetEv;
	// Thermal radiance through TSR needs RGBA16F output/history (R11G11B10 quantizes it far above the detector NETD).
	ThermalTsrAlpha.Update(bThermal, FThermalTsrAlpha::FindCVar());

	// One command: a frame never pairs IR thermal parameters with EO sensor parameters (or the reverse), nor thermal parameters
	// with another tick's land-cover window (ROADMAP 4B).
	TSharedPtr<FCamSimFrameGrabExtension, ESPMode::ThreadSafe> Ext = GrabExtension;
	TSharedPtr<const FThermalFrameParams, ESPMode::ThreadSafe> ConstThermal = ThermalParams;
	ENQUEUE_RENDER_COMMAND(CamSimSensorParams)([Ext, Params, ConstThermal, LandCoverGpu](FRHICommandListImmediate&)
	{
		Ext->SetParams_RenderThread(Params);
		Ext->SetThermalParams_RenderThread(ConstThermal, LandCoverGpu);
	});
}

void UCamSimCaptureComponent::SetThermalPose(double LatDeg, double LonDeg, double AltHaeM, const FVector& UpWorld,
	ACesiumGeoreference* Georeference)
{
	ThermalGeoreference = Georeference;
	ThermalLat = LatDeg;
	ThermalLon = LonDeg;
	ThermalAlt = AltHaeM;
	ThermalUp  = UpWorld;
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
	FrameDropStats.EncoderBusy++;
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

TArray<FEntityWaterPlane> UCamSimCaptureComponent::ComputeWaterPlanes(const TArray<FEntityAnnotationData>& Entities) const
{
	const FOceanSurface* Ocean = Subsystem ? Subsystem->GetOceanSurface() : nullptr;   // null: ocean off
	const FCamSimGeospatialProvider* Geo = Subsystem ? Subsystem->GetGeospatialProvider() : nullptr;
	UWorld* World = GetWorld();
	if (!Ocean || !Geo || !World) return {};
	auto GeoToWorld = [Geo, World](double Lat, double Lon, double AltM, FVector& W) { return Geo->GeoToWorld(World, Lat, Lon, AltM, W); };
	return CamSimGroundTruth::BuildEntityWaterPlanes(Entities, *Ocean, GeoToWorld);   // surface vessels only
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
	S.bWantDepth = bAnnotated && DepthReadbackPool.IsValidIndex(Slot) && DepthReadbackPool[Slot];
	S.InstanceIds.Reset();
	S.ReadyStreak     .Store(0, EMemoryOrder::Relaxed);
	S.PollAttempts    .Store(0, EMemoryOrder::Relaxed);
	// SeqCst: the resets above are visible to any poll that sees the new generation.
	S.Generation.Store(Gen, EMemoryOrder::SequentiallyConsistent);

	// The game viewport renders after this tick; the sensor graph runs in place
	// of its tonemapper and the extension copies its NV12 output into this
	// slot's readback. Enqueued now, so it's the newest request when this
	// frame's scene render runs.
	TSharedPtr<FCamSimFrameGrabExtension, ESPMode::ThreadSafe> Ext = GrabExtension;
	TAtomic<uint32>* Grabbed = &GrabbedGeneration[Slot];
	TAtomic<uint32>* IdGrabbed = &IdGrabbedGeneration[Slot];
	FRHIGPUBufferReadback* Nv12Readback = Nv12ReadbackPool[Slot].Get();
	FRHIGPUBufferReadback* IdReadback = S.bWantIds ? IdReadbackPool[Slot].Get() : nullptr;
	FRHIGPUBufferReadback* DepthReadback = S.bWantDepth ? DepthReadbackPool[Slot].Get() : nullptr;
	TAtomic<uint32>* DepthGrabbed = &DepthGrabbedGeneration[Slot];
	FFrameGrabRequest Req;
	Req.FrameIndex   = FrameIdx;
	Req.Generation   = Gen;
	Req.TargetIndex  = Slot;
	Req.bInstanceIds = S.bWantIds;
	Req.bDepth       = S.bWantDepth;
	// Per-entity water planes for the submerged-hull cut, at this frame's sea state (game thread, doubles).
	if (S.bWantIds) Req.WaterPlanes = ComputeWaterPlanes(S.Entities);
	ENQUEUE_RENDER_COMMAND(CamSimRequestGrab)(
		[Ext, Nv12Readback, IdReadback, Req, Grabbed, IdGrabbed, DepthReadback, DepthGrabbed]
		(FRHICommandListImmediate&)
	{
		if (!Ext) return;
		Ext->PushRequest_RenderThread(Req, Nv12Readback, IdReadback, Grabbed, IdGrabbed, DepthReadback, DepthGrabbed);
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
			FrameDropStats.ReadbackTimeout++;
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
	// The size the sensor graph was set up with.
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
	FRHIGPUBufferReadback*  Nv12Readback = Nv12ReadbackPool.IsValidIndex(Slot) ? Nv12ReadbackPool[Slot].Get() : nullptr;

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
	// Depth map: one float (metres) per output pixel.
	FRHIGPUBufferReadback* DepthReadback = (S.bWantDepth && DepthReadbackPool.IsValidIndex(Slot)) ? DepthReadbackPool[Slot].Get() : nullptr;
	TAtomic<uint32>* DepthGrabbed = &DepthGrabbedGeneration[Slot];
	const int32  DepthPixels    = GpuSensorSize.X * GpuSensorSize.Y;

	ENQUEUE_RENDER_COMMAND(CamSimPollReadback)(
		[RingPtr, &S, Slot, Nv12Readback, Nv12Bytes, ReadyPollsRequired,
		 CaptureGen, Grabbed, IdReadback, IdGrabbed, IdBytes, DepthReadback, DepthGrabbed, DepthPixels]
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
			[Nv12Readback, IdReadback, IdGrabbed, DepthReadback, DepthGrabbed, CaptureGen]()
			{
				if (!Nv12Readback || !Nv12Readback->IsReady()) return false;
				const bool bWaitIds = CamSimReadback::ShouldWaitForIds(IdReadback != nullptr,
					IdGrabbed->Load(EMemoryOrder::SequentiallyConsistent), CaptureGen);
				const bool bWaitDepth = CamSimReadback::ShouldWaitForIds(DepthReadback != nullptr,
					DepthGrabbed->Load(EMemoryOrder::SequentiallyConsistent), CaptureGen);
				return (!bWaitIds || IdReadback->IsReady()) && (!bWaitDepth || DepthReadback->IsReady());
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

		// Depth map: like the IDs, present only when its copy was issued for this capture.
		S.Depth.Reset();
		if (CamSimReadback::ShouldWaitForIds(DepthReadback != nullptr, DepthGrabbed->Load(EMemoryOrder::SequentiallyConsistent), CaptureGen))
		{
			const uint32 DepthBytes = static_cast<uint32>(DepthPixels * sizeof(float));
			if (const void* Raw = DepthReadback->Lock(DepthBytes))  // null: do NOT Unlock
			{
				S.Depth.SetNumUninitialized(DepthPixels);
				FMemory::Memcpy(S.Depth.GetData(), Raw, DepthBytes);
				DepthReadback->Unlock();
			}
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
	FEncoderThread*          EncThread = EncoderThread.Get();  // outlives the task
	FPipelineLatencyTracker* LT        = LatencyTracker;       // outlives the task
	const FIntPoint          IdSize    = GpuSensorSize;        // the ID buffer's size (fixed at Initialize)

	// The sensor model already ran on the GPU (3B). The ML ground-truth writers
	// (file I/O) stay off the game thread; bSensorBusy is cleared after them,
	// not after encode.
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
		[this, EncThread, Collector, LT,
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
				// Its own task: PNG-compressing a real depth map must not hold up the next capture.
				if (++DepthWritesInFlight <= MaxDepthWritesInFlight)
				{
					AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
						[this, Collector, IdSize, FrameIdx, Depth = MoveTemp(Depth)]()
					{
						Collector->WriteDepthFrame(Depth, IdSize.X, IdSize.Y, FrameIdx);
						--DepthWritesInFlight;
					});
				}
				else
				{
					--DepthWritesInFlight;
					const uint64 Skipped = ++DepthWritesSkipped;
					if (Skipped == 1 || Skipped % 300 == 0)
					{
						UE_LOG(LogCamSim, Warning, TEXT("GroundTruth: depth-map writes can't keep up (%d in flight); ")
							TEXT("skipped %llu depth frame(s) so far"), MaxDepthWritesInFlight, Skipped);
					}
				}
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
