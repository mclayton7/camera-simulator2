// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HAL/ThreadSafeBool.h"
#include "Metadata/CamSimTelemetry.h"
#include "Sensor/SensorTypes.h"      // ESensorMode
#include "Sensor/IPixelPipeline.h"   // IPixelPipeline
// FRHIGPUTextureReadback needs a complete type here because the UHT-generated
// .gen.cpp instantiates TArray<TUniquePtr<FRHIGPUTextureReadback>>'s destructor.
#include "RHIGPUReadback.h"
#include "Camera/ReadbackRing.h"
#include "CamSimCaptureComponent.generated.h"

class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class UMaterialInterface;
class UMaterialParameterCollection;
class UCamSimSubsystem;
class FEncoderThread;
class FCamSimFrameGrabExtension;
struct FPipelineLatencyTracker;
struct FCamSimConfig;

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
	int32 Total() const { return EncoderBusy.Load() + ReadbackTimeout.Load() + SocketError.Load(); }
};

/**
 * UCamSimCaptureComponent
 *
 * The capture pipeline behind ACamSimCamera's sensor view:
 *   1. SceneCapture2D renders into a ring of three render targets
 *      (+ an optional depth capture for ML ground truth);
 *   2. an async GPU→CPU readback, polled from the render thread with no
 *      FlushRenderingCommands();
 *   3. the CPU sensor model (IPixelPipeline) and ground-truth writers on a
 *      background task;
 *   4. an SPSC hand-off to the persistent encoder thread.
 * Also applies the scene capture's render settings (optical realism,
 * rendering quality, the GPU sensor material). Driven by ACamSimCamera on the
 * game thread; does not tick itself.
 */
UCLASS()
class CAMSIMTEST_API UCamSimCaptureComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCamSimCaptureComponent();

	/**
	 * Create render targets, readback pools, the sensor pipeline and the
	 * encoder thread, and apply render settings to Sensor.
	 */
	void Initialize(USceneCaptureComponent2D* InSensor, UCamSimSubsystem* InSubsystem, const FCamSimConfig& Cfg);

	/** Stop the encoder thread and release readback resources (flushes rendering commands). */
	void Shutdown();

	/** Consume a finished readback and hand frames to the sensor task. Call every tick. */
	void Poll();

	/** True when a readback slot is free, so Capture() may run (ROADMAP 3A.1: up to three in flight). */
	bool IsReadyForCapture() const;

	/** Count a frame skipped because every readback slot was busy (the sensor/encoder is behind). */
	void NoteCaptureSkipped();

	/** Capture the scene now, tagged with Telemetry, and start its async readback. */
	void Capture(const FCamSimTelemetry& Telemetry);

	/** True if this render frame should be skipped to honour the output frame rate. */
	bool ShouldSkipFrameForDecimation(const FCamSimConfig& Cfg);

	/** Push per-frame parameters to the GPU sensor material (no-op without it). */
	void UpdateGpuSensorParams(ESensorMode Mode, const FCamSimConfig& Cfg);

	IPixelPipeline* GetSensorPipeline() const { return SensorFX.Get(); }
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
	void SnapshotGroundTruthEntities();
	/** One render-thread poll per in-flight slot. */
	void EnqueuePolls();
	void EnqueuePoll(int32 Slot);
	void SubmitFrameToEncoder(TArray<FColor> PixelData, FCamSimTelemetry Telemetry,
	                          uint64 FrameIdx, TArray<float> DepthMetres);

	UPROPERTY(Transient)
	TObjectPtr<USceneCaptureComponent2D> Sensor;

	UPROPERTY(Transient)
	TObjectPtr<UCamSimSubsystem> Subsystem;

	/** Ring of render targets: frame N+2 renders while N+1 reads back and N encodes. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextureRenderTarget2D>> RenderTargets;

	/** Optional depth capture for ML training data (Phase 17A). */
	UPROPERTY(Transient)
	TObjectPtr<USceneCaptureComponent2D> DepthCapture;

	/** Depth render targets, one per readback slot (PF_R32_FLOAT). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextureRenderTarget2D>> DepthRenderTargets;

	// Phase 27A — GPU sensor post-process
	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> GpuSensorMpc;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> GpuSensorMat;

	/**
	 * One readback helper per slot, so the EnqueueCopy of a newer frame can't
	 * race the Lock/Unlock still targeting an older one.
	 */
	TArray<TUniquePtr<FRHIGPUTextureReadback>> ColorReadbackPool;
	TArray<TUniquePtr<FRHIGPUTextureReadback>> DepthReadbackPool;

	/** ROADMAP 3A: grabs the game viewport when the sensor is the primary view (null otherwise). */
	TSharedPtr<FCamSimFrameGrabExtension, ESPMode::ThreadSafe> GrabExtension;
	bool bPrimaryView = false;

	/** Create the grab extension once the game viewport exists. False while it doesn't. */
	bool EnsureGrabExtension();

	/**
	 * Per render-target slot: the capture generation whose copy the grab
	 * extension really issued. The poll trusts a slot's fence only when this
	 * matches the capture's generation.
	 * writer: render (grab pass) → reader: render (poll) (SeqCst).
	 */
	TAtomic<uint32> GrabbedGeneration[FReadbackRing::NumSlots];

	/** CPU-side sensor post-processing pipeline (Phase 11). */
	TUniquePtr<IPixelPipeline> SensorFX;

	/** Persistent encoder thread — drains processed frames from an SPSC queue. */
	TUniquePtr<FEncoderThread, FEncoderThreadDeleter> EncoderThread;

	FPipelineLatencyTracker* LatencyTracker = nullptr;

	// -----------------------------------------------------------------------
	// Threading: game-thread-only state is plain members. Cross-thread
	// counters and flags are atomics annotated "writer → readers (order)".
	// "SeqCst" pairs a flag with data writes (release/acquire).
	// -----------------------------------------------------------------------

	/**
	 * True while the sensor task is processing a frame; cleared by the task
	 * after depositing into the encoder queue.
	 * writer: game + sensor task → readers: game (relaxed; no paired data).
	 */
	FThreadSafeBool bSensorBusy;

	/**
	 * ROADMAP 3A.1 — readback ring. A capture takes the next free slot, so up
	 * to NumSlots readbacks are in flight and a frame can be captured every
	 * tick. Slot states and in-order delivery live in FReadbackRing; per-slot
	 * data below. The render thread writes a slot's Pixels/Depth, then marks it
	 * Complete (SeqCst); the game thread's SeqCst load in PeekFinished makes the
	 * arrays visible.
	 */
	FReadbackRing Ring;

	struct FSlot
	{
		FCamSimTelemetry Telemetry;               // game thread
		/** This capture's generation: stale poll commands for a reused slot no-op. writer: game → reader: render. */
		TAtomic<uint32>  Generation       { 0 };
		TArray<FColor>   Pixels;                  // render writes → game reads after Complete
		TArray<float>    Depth;
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
