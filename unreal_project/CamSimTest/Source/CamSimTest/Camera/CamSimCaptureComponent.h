// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HAL/ThreadSafeBool.h"
#include "Metadata/CamSimTelemetry.h"
#include "Sensor/SensorTypes.h"      // ESensorMode
// FRHIGPUTextureReadback needs a complete type here because the UHT-generated
// .gen.cpp instantiates TArray<TUniquePtr<FRHIGPUTextureReadback>>'s destructor.
#include "RHIGPUReadback.h"
#include "Camera/ReadbackRing.h"
#include "Camera/SensorGpuTimer.h"
#include "Sensor/SensorController.h"
#include "Sensor/SensorStatsMailbox.h"
#include "GroundTruth/AnnotationTypes.h"
#include "Thermal/ThermalFrameBuilder.h"
#include "Camera/ThermalTsrAlpha.h"
#include "CamSimCaptureComponent.generated.h"

class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class UCamSimSubsystem;
class FEncoderThread;
class FCamSimFrameGrabExtension;
class FCamSimSnapshotService;
struct FPipelineLatencyTracker;
struct FCamSimConfig;
struct FEntityWaterPlane;

/**
 * Forward-declared deleter so the component can hold a
 * `TUniquePtr<FEncoderThread, FEncoderThreadDeleter>` without including
 * `Encoder/EncoderThread.h`. UHT's .gen.cpp instantiates every member's
 * destructor; a plain TUniquePtr would inline `delete` against an incomplete
 * type there (-Werror,-Wdelete-incomplete). operator() is defined in the .cpp.
 */
struct FEncoderThreadDeleter
{
	void operator()(FEncoderThread* Ptr) const;
};

/**
 * Phase 27B — per-category frame drop counters.
 *
 * Each counter is written from the thread that first observes the drop and
 * read from the HTTP thread. Relaxed ordering: pure monotonic counters.
 */
struct FFrameDropStats
{
	TAtomic<int32> EncoderBusy     { 0 };  // writer: game; readers: HTTP
	TAtomic<int32> ReadbackTimeout { 0 };  // writer: game (observed from render); readers: HTTP
	TAtomic<int32> SocketError     { 0 };  // writer: encoder thread; readers: HTTP
	/** ROADMAP 3B: sensor histogram went stale (AE held). Not a frame drop, so not in Total(). */
	TAtomic<int32> SensorStatsStale { 0 };  // writer: game; readers: HTTP
	int32 Total() const { return EncoderBusy.Load() + ReadbackTimeout.Load() + SocketError.Load(); }
};

/**
 * UCamSimCaptureComponent
 *
 * The capture pipeline behind ACamSimCamera's sensor view:
 *   1. the GPU sensor graph replaces UE's tonemapper in the game viewport
 *      (FCamSimFrameGrabExtension) and writes an NV12 frame per request into a
 *      ring of three readback slots (+ an optional depth capture for ML ground
 *      truth);
 *   2. an async GPU→CPU readback, polled from the render thread with no
 *      FlushRenderingCommands();
 *   3. ground-truth writers on a background task;
 *   4. an SPSC hand-off to the persistent encoder thread.
 * Also applies the sensor view's render settings (optical realism, rendering
 * quality, manual exposure). Driven by ACamSimCamera on the game thread; does
 * not tick itself.
 */
UCLASS()
class CAMSIMTEST_API UCamSimCaptureComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCamSimCaptureComponent();

	/**
	 * Create the NV12 (and depth) readback pools and the encoder thread, and
	 * apply render settings to Sensor. Produces no frames when the subsystem
	 * reports the sensor graph unavailable.
	 */
	void Initialize(USceneCaptureComponent2D* InSensor, UCamSimSubsystem* InSubsystem, const FCamSimConfig& Cfg);

	/** Stop the encoder thread and release readback resources (flushes rendering commands). */
	void Shutdown();

	/** Consume a finished readback and hand frames to the background task. Call every tick. */
	void Poll();

	/** True when a readback slot is free, so Capture() may run (ROADMAP 3A.1: up to three in flight). */
	bool IsReadyForCapture() const;

	/** Count a frame skipped because every readback slot was busy (the sensor/encoder is behind). */
	void NoteCaptureSkipped();

	/** Request the sensor graph's output for the next game-viewport frame, tagged with Telemetry. */
	void Capture(const FCamSimTelemetry& Telemetry);

	/** True if this render frame should be skipped to honour the output frame rate. */
	bool ShouldSkipFrameForDecimation(const FCamSimConfig& Cfg);

	/**
	 * ROADMAP 3B: run the sensor controller (AE / IR AGC) on the newest GPU
	 * histogram and send this tick's parameters to the sensor graph. No-op
	 * without the sensor graph. Call every tick, before Poll(). LiveHFovDeg is
	 * this frame's horizontal FOV (focal length of the lens model, ROADMAP 3B.2).
	 */
	void UpdateSensorParams(ESensorMode Mode, uint8 Polarity, bool bCameraCut, float LiveHFovDeg, const FCamSimConfig& Cfg);

	/** ROADMAP 4A: fixed UE AutoExposureBias while the thermal pass runs (ThermalCS divides View.PreExposure out). */
	static constexpr float ThermalUeExposureEv = -12.0f;
	/**
	 * ROADMAP 4A: whether this tick's IR frame is thermal radiance. bThermalAvailable = the subsystem's startup decision
	 * AND the live thermal.enabled (a hot reload to false restores the luminance proxy).
	 */
	static bool ShouldRunThermal(ESensorMode Mode, bool bThermalAvailable) { return Mode == ESensorMode::IR && bThermalAvailable; }
	/** ROADMAP 4A: the camera's geodetic pose (WGS-84, HAE m) and geodetic up in UE world space. Call every tick before UpdateSensorParams. */
	void SetThermalPose(double LatDeg, double LonDeg, double AltHaeM, const FVector& UpWorld);

	/** Whether the GPU sensor graph runs this session (fixed at Initialize). */
	bool HasSensorGraph() const { return bSensorGraph; }
	/** GPU time of the sensor graph in ms; -1 when unknown or without the graph. */
	float GetSensorGpuMs() const { return GpuTimer.GetLatestMs(); }
	/** GPU time of ThermalCS in ms; -1 unless the thermal pass ran last tick (ROADMAP 4A). */
	float GetThermalGpuMs() const { return bThermalActiveLastTick ? GpuTimer.GetThermalLatestMs() : -1.0f; }
	/** Log2 of the sensor gain emitted last tick. */
	float GetSensorGainEv() const { return SensorController.GetGainEv(); }
	/** Median scene signal (log2) of the last histogram the controller used. */
	float GetSceneMedianLog2() const { return SensorController.GetLastMedianLog2(); }

	void SetLatencyTracker(FPipelineLatencyTracker* Tracker);

	// Stats
	const FFrameDropStats& GetFrameDropStats() const { return FrameDropStats; }
	bool   IsTrackingFrameDrops() const { return bTrackFrameDrops; }
	/** Frames dropped because the encoder thread's queue was full. */
	uint64 GetDroppedFrameCount() const;
	uint64 GetFramesCaptured() const { return FrameIndex; }
	bool   IsSensorBusy() const { return bSensorBusy; }
	bool   IsReadbackInFlight() const { return Ring.NumInFlight() > 0; }
	int32  GetReadbacksInFlight() const { return Ring.NumInFlight(); }
	int32  GetCaptureTargetIndex() const { return Ring.GetNextSlot(); }
	int32  GetPendingReadbackTargetIndex() const { return Ring.NumInFlight() > 0 ? Ring.GetOrder()[0] : INDEX_NONE; }

private:
	void ApplyRenderSettings(const FCamSimConfig& Cfg);
	void CreateDepthCapture(const FCamSimConfig& Cfg);
	/** Build this frame's entity annotation snapshot (empty when the collector is off). */
	TArray<FEntityAnnotationData> BuildGroundTruthSnapshot() const;
	/** Per tagged entity, the sea-surface plane InstanceIdCS cuts its submerged hull at; empty when the ocean is off. */
	TArray<FEntityWaterPlane> ComputeWaterPlanes(const TArray<FEntityAnnotationData>& Entities) const;
	/** One render-thread poll per in-flight slot. */
	void EnqueuePolls();
	void EnqueuePoll(int32 Slot);
	struct FSlot;
	/** Offer a delivered frame to a snapshot service (NV12 converted to BGRA). */
	void OfferSnapshot(FCamSimSnapshotService& Snap, const FSlot& S) const;
	void SubmitFrameToEncoder(TArray<uint8> Nv12, FCamSimTelemetry Telemetry, uint64 FrameIdx, TArray<float> DepthMetres,
	                          TArray<FEntityAnnotationData> Entities, TArray<uint32> InstanceIds);

	UPROPERTY(Transient)
	TObjectPtr<USceneCaptureComponent2D> Sensor;

	UPROPERTY(Transient)
	TObjectPtr<UCamSimSubsystem> Subsystem;

	/** Optional depth capture for ML training data (Phase 17A). */
	UPROPERTY(Transient)
	TObjectPtr<USceneCaptureComponent2D> DepthCapture;

	/** Depth render targets, one per readback slot (PF_R32_FLOAT). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextureRenderTarget2D>> DepthRenderTargets;

	/**
	 * One depth readback helper per slot, so the EnqueueCopy of a newer frame
	 * can't race the Lock/Unlock still targeting an older one.
	 */
	TArray<TUniquePtr<FRHIGPUTextureReadback>> DepthReadbackPool;

	/** ROADMAP 3A/3B: runs the sensor graph in the game viewport (null until the viewport exists). */
	TSharedPtr<FCamSimFrameGrabExtension, ESPMode::ThreadSafe> GrabExtension;

	/** Create the grab extension once the game viewport exists. False while it doesn't. */
	bool EnsureGrabExtension();

	// ROADMAP 3B — GPU sensor model (game thread unless noted)
	/** The GPU sensor graph runs this session (UCamSimSubsystem::IsSensorGraphAvailable, fixed at Initialize). */
	bool bSensorGraph = false;
	/** Size the sensor graph and NV12 readbacks were set up with (never the live, hot-reloadable config). */
	FIntPoint GpuSensorSize = FIntPoint::ZeroValue;
	/** Consecutive failed readbacks (usually never grabbed); logged once at GpuStallLogThreshold. */
	int32 ConsecutiveGpuFailures = 0;
	bool  bLoggedGpuStall = false;
	static constexpr int32 GpuStallLogThreshold = 60;
	/** One NV12 readback per ring slot. */
	TArray<TUniquePtr<FRHIGPUBufferReadback>> Nv12ReadbackPool;
	/**
	 * Ground truth (ROADMAP 2.7): one instance-ID readback per ring slot, created only
	 * when ml_training.enabled && bounding_boxes and InstanceIdCS is available. Empty =
	 * no IDs requested (annotations use projected boxes).
	 */
	TArray<TUniquePtr<FRHIGPUBufferReadback>> IdReadbackPool;
	/** Collector's annotation interval (fixed at Initialize, as the collector's): IDs only on annotated frames. */
	int32 IdIntervalFrames = 1;
	FSensorController   SensorController;
	/** Histograms, render thread → game thread. */
	FSensorStatsMailbox StatsMailbox;
	/** Render thread (via the extension), except GetLatestMs. */
	FSensorGpuTimer     GpuTimer;
	uint32 ParamsSerial = 0;
	/** The thermal pass was requested last tick (IR, thermal available and enabled, inputs gathered). */
	bool bThermalActiveLastTick = false;
	/**
	 * ROADMAP 4A: per-frame thermal parameters (game thread). Configure runs every thermal tick with the live config:
	 * it rebuilds the LUT only on a band change and the material table only when thermal.materials changes, so hot
	 * reloads of the band, materials and the other thermal.* keys apply on the next frame.
	 */
	FThermalFrameBuilder ThermalBuilder;
	/** r.TSR.AlphaChannel = 1 while thermal runs (RGBA16F TSR output/history for the radiance), restored otherwise. */
	FThermalTsrAlpha ThermalTsrAlpha;
	double ThermalLat = 0.0, ThermalLon = 0.0, ThermalAlt = 0.0;
	FVector ThermalUp = FVector::UpVector;
	double LastSensorUpdateSimSec = -1.0;
	/**
	 * Optics fields of FSensorFrameParams (CamSimOptics::SetOptics) for the key below: recomputed
	 * only when the mode's lens or the live HFOV changes (the PSF taps need erf).
	 */
	struct FOpticsCache
	{
		bool   bValid = false;
		ESensorMode Mode = ESensorMode::EO;
		FSensorOpticsConfig Lens;
		float  HFovDeg = 0.0f;
		bool   bConverges = true;
		FSensorFrameParams Params;   // only the optics fields are used
	} OpticsCache;
	/** A live FOV where the lens does not converge was logged; cleared once it converges again. */
	bool bLoggedDistortionFallback = false;
	/**
	 * EV added to the sensor gain for UE's manual AutoExposureBias, so the
	 * view's pre-exposure tracks the gain and scene colour stays in fp16 range.
	 * Measured 2026-09-27 (M1 Pro, orbit): with AEM_Manual and physical camera
	 * exposure off, View.PreExposure == 2^AutoExposureBias exactly
	 * (log2 PreExposure = log2 Gain = -15.5 in daylight), so no offset is needed.
	 */
	static constexpr float UeExposureOffsetEv = 0.0f;

	/**
	 * Per readback-ring slot: the capture generation whose copy the grab
	 * extension really issued. The poll trusts a slot's fence only when this
	 * matches the capture's generation.
	 * writer: render (grab pass) → reader: render (poll) (SeqCst).
	 */
	TAtomic<uint32> GrabbedGeneration[FReadbackRing::NumSlots];
	/**
	 * Per slot: the generation whose instance-ID copy was issued (stored before
	 * GrabbedGeneration). Lagging GrabbedGeneration = no IDs this frame.
	 * writer: render (grab pass) → reader: render (poll) (SeqCst).
	 */
	TAtomic<uint32> IdGrabbedGeneration[FReadbackRing::NumSlots];

	/** Persistent encoder thread — drains processed frames from an SPSC queue. */
	TUniquePtr<FEncoderThread, FEncoderThreadDeleter> EncoderThread;

	FPipelineLatencyTracker* LatencyTracker = nullptr;

	// -----------------------------------------------------------------------
	// Threading: game-thread-only state is plain members. Cross-thread
	// counters and flags are atomics annotated "writer → readers (order)".
	// "SeqCst" pairs a flag with data writes (release/acquire).
	// -----------------------------------------------------------------------

	/**
	 * True while the background task is processing a frame (ground truth);
	 * cleared by the task after depositing into the encoder queue.
	 * writer: game + background task → readers: game (relaxed; no paired data).
	 */
	FThreadSafeBool bSensorBusy;

	/**
	 * ROADMAP 3A.1 — readback ring. A capture takes the next free slot, so up
	 * to NumSlots readbacks are in flight and a frame can be captured every
	 * tick. Slot states and in-order delivery live in FReadbackRing; per-slot
	 * data below. The render thread writes a slot's Nv12/Depth, then marks it
	 * Complete (SeqCst); the game thread's SeqCst load in PeekFinished makes the
	 * arrays visible.
	 */
	FReadbackRing Ring;

	struct FSlot
	{
		FCamSimTelemetry Telemetry;               // game thread
		/** This capture's generation: stale poll commands for a reused slot no-op. writer: game → reader: render. */
		TAtomic<uint32>  Generation       { 0 };
		TArray<uint8>    Nv12;                    // render writes → game reads after Complete
		TArray<float>    Depth;
		TArray<FEntityAnnotationData> Entities;   // game thread fills at capture; moved to the background task
		TArray<uint32>   InstanceIds;             // render writes → game reads after Complete (empty = none)
		bool             bWantIds = false;        // game thread at capture; captured by value into the poll
		// Reset by the game thread at capture, advanced by the render thread's polls.
		TAtomic<uint8>   ReadyStreak      { 0 };  // "N consecutive Ready polls before consuming"
		TAtomic<uint8>   DepthReadyStreak { 0 };
		TAtomic<uint32>  PollAttempts     { 0 };  // past MaxReadbackPolls the slot fails
	};
	FSlot Slots[FReadbackRing::NumSlots];

	static constexpr uint32 MaxReadbackPolls = 60;  // ~2 s at 30 Hz

	uint32 NextGeneration = 0;  // game thread

	/** Frame counter for PTS calculation. */
	uint64 FrameIndex = 0;

	/** Phase 27B — per-category frame drop counters. */
	FFrameDropStats FrameDropStats;
	bool            bTrackFrameDrops = false;

	// Output decimation — render at RenderFrameRateHz, encode at OutputFrameRateHz
	uint64 RenderFrameCounter = 0;
};
