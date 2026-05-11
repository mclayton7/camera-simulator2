// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/ThreadSafeBool.h"
#include "Metadata/KlvBuilder.h"
#include "Sensor/SensorTypes.h"      // ESensorMode
#include "Sensor/IPixelPipeline.h"   // IPixelPipeline
// FEncoderThread is intentionally held behind a TUniquePtr with a custom
// forward-declared deleter (see `FEncoderThreadDeleter` below). UHT's
// generated .gen.cpp emits DEFINE_VTABLE_PTR_HELPER_CTOR_NS(, ACamSimCamera),
// which forces the destructor of every member to be instantiable in that TU.
// A plain TUniquePtr<FEncoderThread> would inline `delete Ptr;` there and
// fail with -Werror,-Wdelete-incomplete. The custom deleter declares
// operator() here but defines it in CamSimCamera.cpp, so the .gen.cpp TU
// only emits a call instruction — link-time resolution sees the full type.
#include "Diagnostics/PipelineLatencyTracker.h"
// FRHIGPUTextureReadback needs a complete type here because the UHT-generated
// CamSimCamera.gen.cpp instantiates TArray<TUniquePtr<FRHIGPUTextureReadback>>'s
// destructor outside this TU.
#include "RHIGPUReadback.h"
#include "CamSimCamera.generated.h"

class USceneCaptureComponent2D;
class UCesiumGlobeAnchorComponent;
class UCamSimSubsystem;
class UCamSimGimbalComponent;
class UCamSimSensorComponent;
class UMaterialInterface;
class UMaterialParameterCollection;
class FEncoderThread;

/**
 * Forward-declared deleter that lets ACamSimCamera hold a
 * `TUniquePtr<FEncoderThread, FEncoderThreadDeleter>` without including
 * `Encoder/EncoderThread.h`. The operator() is DECLARED here and DEFINED in
 * CamSimCamera.cpp, so the UHT-generated .gen.cpp only emits a call to it —
 * no `delete` is instantiated against an incomplete type.
 */
struct FEncoderThreadDeleter
{
	void operator()(FEncoderThread* Ptr) const;
};

/**
 * Phase 27B — per-category frame drop counters.
 *
 * Threading: each counter is written from the thread that first observes the
 * drop (EncoderBusy / SocketError from the game thread; ReadbackTimeout from
 * the render thread). Reads come from the HTTP thread via the health server.
 * Relaxed ordering is sufficient — drops are pure monotonic counters and
 * don't gate any other data.
 */
struct FFrameDropStats
{
	TAtomic<int32> EncoderBusy     { 0 };  // writer: game; readers: HTTP
	TAtomic<int32> ReadbackTimeout { 0 };  // writer: game (observed from render); readers: HTTP
	TAtomic<int32> SocketError     { 0 };  // writer: encoder thread; readers: HTTP
	int32 Total() const { return EncoderBusy.Load() + ReadbackTimeout.Load() + SocketError.Load(); }
};

/**
 * ACamSimCamera
 *
 * A world-partition-friendly actor that:
 *   1. Receives geospatial pose updates from the CIGI receiver (via Tick).
 *   2. Triggers a SceneCapture2D render.
 *   3. Reads back BGRA pixels from the GPU and dispatches them to the
 *      configured IFrameSink running on a background task thread.
 *
 * Place one instance in the persistent level.  The actor self-registers with
 * UCamSimSubsystem so the subsystem can drive it without hard coupling.
 */
UCLASS()
class CAMSIMTEST_API ACamSimCamera : public AActor
{
	GENERATED_BODY()

public:
	ACamSimCamera();

	// AActor interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaTime) override;

	// Called by Tick after geospatial position has been applied
	void CaptureAndEncode();

	// Phase 27B — expose drop stats to subsystem for health JSON
	const FFrameDropStats& GetFrameDropStats() const { return FrameDropStats_; }
	bool IsTrackingFrameDrops()                const { return bTrackFrameDrops_; }

	/** Return a snapshot of the current telemetry for external consumers (e.g. CoT sender). */
	FCamSimTelemetry GetCurrentTelemetry() const { return CurrentTelemetry; }

	/** Read-only accessor for the sensor component (needed by UCamSimSubsystem for
	 *  SensorExtendedResponse opcode 107). */
	UCamSimSensorComponent* GetSensorComp() const { return SensorComp; }

	/** Phase 28G: set the pipeline latency tracker (owned by subsystem, nullable). */
	void SetLatencyTracker(FPipelineLatencyTracker* Tracker) { LatencyTracker_ = Tracker; }

private:
	/** Explicit root scene component. */
	UPROPERTY(VisibleAnywhere, Category = "CamSim")
	TObjectPtr<USceneComponent> Root;

	/** Cesium globe anchor — wraps UE world-space around WGS-84 coordinates. */
	UPROPERTY(VisibleAnywhere, Category = "CamSim")
	TObjectPtr<UCesiumGlobeAnchorComponent> GlobeAnchor;

	/** SceneCapture component pointed at the Cesium globe. */
	UPROPERTY(VisibleAnywhere, Category = "CamSim")
	TObjectPtr<USceneCaptureComponent2D> SceneCapture;

	/** Owns gimbal state (yaw/pitch/roll), slew logic and axis limits. */
	UPROPERTY(VisibleAnywhere, Category = "CamSim")
	TObjectPtr<UCamSimGimbalComponent> GimbalComp;

	/** Owns sensor on/off, waveband, polarity and FOV-preset selection. */
	UPROPERTY(VisibleAnywhere, Category = "CamSim")
	TObjectPtr<UCamSimSensorComponent> SensorComp;

	/** Ping-pong render targets used for capture/readback isolation. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextureRenderTarget2D>> RenderTargets;

	/** Index of the render target used for the next SceneCapture. */
	int32 CaptureTargetIndex = 0;

	/** Index of the render target currently in-flight for readback. */
	int32 PendingReadbackTargetIndex = INDEX_NONE;

	/** Optional depth capture component for ML training data (Phase 17A). */
	UPROPERTY(VisibleAnywhere, Category = "CamSim")
	TObjectPtr<USceneCaptureComponent2D> DepthCapture;

	/** Ping-pong render targets for async depth readback (PF_R32_FLOAT). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextureRenderTarget2D>> DepthRenderTargets;

	int32 DepthCaptureTargetIndex = 0;

	/**
	 * Pool of depth-readback helpers — one per DepthRenderTarget so EnqueueCopy
	 * on frame N+1 can't race Lock/Unlock on frame N. Indexed by PendingDepthReadbackTargetIndex.
	 */
	TArray<TUniquePtr<FRHIGPUTextureReadback>> DepthReadbackPool;

	/** Render-target index that the currently in-flight depth readback is bound to. */
	int32 PendingDepthReadbackTargetIndex = INDEX_NONE;

	/** Cached pointer to our subsystem (set in BeginPlay). */
	UPROPERTY(Transient)
	TObjectPtr<UCamSimSubsystem> Subsystem;

	/** Frame counter for PTS calculation. */
	uint64 FrameIndex = 0;

	// -----------------------------------------------------------------------
	// Threading policy (summary for the TAtomic/FThreadSafeBool fields below):
	//
	//   * Game-thread-only state stays as plain members (FrameIndex,
	//     CaptureTargetIndex, etc.) — no synchronisation needed.
	//   * Cross-thread counters and one-shot flags use TAtomic<T>. Each field
	//     below is annotated "writer → readers (order)".
	//   * bSensorBusy uses FThreadSafeBool for historical reasons — it's a
	//     32-bit relaxed atomic bool with implicit conversions back to bool,
	//     so `if (!bSensorBusy)` works without `.Load()`. Kept as-is to avoid
	//     churn in every call site.
	//
	// "SeqCst" below means Load/Store(EMemoryOrder::SequentiallyConsistent);
	// the matching read on the opposite thread takes SeqCst too, which is what
	// we use whenever a flag is paired with data writes (release/acquire).
	// -----------------------------------------------------------------------

	/**
	 * Set to true while the sensor post-process task is running on the previous frame.
	 * Cleared by the sensor async task after depositing into the encoder queue.
	 * (Replaces the old bEncoderBusy — encode now runs on a separate persistent thread.)
	 * writer: game + sensor task → readers: game (relaxed; no paired data).
	 */
	FThreadSafeBool bSensorBusy;

	/** Persistent encoder thread — drains processed frames from SPSC queue. */
	TUniquePtr<FEncoderThread, FEncoderThreadDeleter> EncoderThread;

	/**
	 * Pool of GPU→CPU readback helpers — one per RenderTarget so the EnqueueCopy
	 * triggered on frame N+1 can't race the Lock/Unlock that still targets
	 * frame N's staging copy. Indexed by PendingReadbackTargetIndex.
	 */
	TArray<TUniquePtr<FRHIGPUTextureReadback>> ColorReadbackPool;

	/**
	 * Async readback result hand-off (render thread writes → game thread reads).
	 * Render command fills AsyncPixels_/AsyncDepth_, then transitions
	 * ReadbackState_ to Complete with sequentially-consistent semantics; the
	 * matching SeqCst load on the game thread makes the array contents visible
	 * without FlushRenderingCommands().
	 */
	TArray<FColor> AsyncPixels_;
	TArray<float>  AsyncDepth_;

	/**
	 * Readback state machine. Replaces the previous four-flag cluster
	 * (bReadbackPending, bReadbackDMAIssued, bPollComplete_, bPollFailed_).
	 * Transitions:
	 *   Idle      → DMAQueued  (game thread, before ENQUEUE_RENDER_COMMAND)
	 *   DMAQueued → Complete   (render thread, after pixels copied)
	 *   DMAQueued → Failed     (render thread, on Lock/Unlock error)
	 *   Complete  → Idle       (game thread, after dispatch)
	 *   Failed    → Idle       (game thread, after logging)
	 *
	 * SeqCst on every Store/Load — the state is paired with the
	 * AsyncPixels_/AsyncDepth_ data writes on the DMAQueued → Complete edge.
	 * writer: game (Idle↔DMAQueued, Complete/Failed→Idle) + render (DMAQueued→Complete/Failed)
	 * readers: both threads (SeqCst).
	 */
	enum class EReadbackState : uint8 { Idle, DMAQueued, Complete, Failed };
	TAtomic<EReadbackState> ReadbackState_ { EReadbackState::Idle };
	/**
	 * Monotonic poll generation — game thread bumps on every new CaptureAndEncode.
	 * Each poll render command captures the generation by value and no-ops if
	 * the generation has advanced, preventing stale polls from one frame from
	 * resurrecting an already-consumed result once we advance to the next frame.
	 * writer: game → reader: render (relaxed; compared for equality only).
	 */
	TAtomic<uint32> PollGeneration_{0};
	uint8          RenderReadyStreak_ = 0;      // render-thread only
	uint8          RenderDepthReadyStreak_ = 0; // render-thread only

	/** Frame index in-flight through the GPU readback pipeline. */
	uint64 PendingFrameIndex = 0;

	/** Intermediate result: readback completed but sensor still busy. */
	TArray<FColor>   CompletedPixels_;
	TArray<float>    CompletedDepth_;
	FCamSimTelemetry CompletedTelemetry_;
	uint64           CompletedFrameIndex_ = 0;
	bool             bReadbackResultReady_ = false;

	/** Telemetry snapshot captured at the same time as the in-flight frame. */
	FCamSimTelemetry PendingTelemetry;

	/** Monotonically increasing count of frames dropped due to encoder busy.
	 *  writer: game → readers: game (heartbeat log) + HTTP /metrics (relaxed). */
	TAtomic<uint64> DroppedFrameCount { 0 };

	/** Telemetry cached from the last applied CIGI state. */
	FCamSimTelemetry CurrentTelemetry;

	/** Cesium camera manager registration IDs (-1 = not registered). */
	int32 CesiumCameraId = -1;
	int32 CesiumPrefetchCameraId = -1;

	// Gimbal and sensor state have been extracted to UCamSimGimbalComponent
	// and UCamSimSensorComponent. Access via GimbalComp / SensorComp.

	/** CPU-side sensor post-processing pipeline (Phase 11). */
	TUniquePtr<IPixelPipeline> SensorFX;

	// Phase 27A — GPU sensor post-process
	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> GpuSensorMpc_;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> GpuSensorMat_;

	/** Phase 27B — per-category frame drop counters. */
	FFrameDropStats FrameDropStats_;
	bool            bTrackFrameDrops_ = false;

	// Phase 26C — Ground speed from position delta (Tag 8)
	double PrevGeoLatDeg_   = 0.0;
	double PrevGeoLonDeg_   = 0.0;
	bool   bHasPrevGeoPos_  = false;

	/** Phase 28G: per-frame pipeline latency tracker (owned by subsystem, nullable). */
	FPipelineLatencyTracker* LatencyTracker_ = nullptr;

	// Phase 27E — Tile prefetch during gimbal slew
	float PrevGimbalPanDeg_   = 0.0f;
	float PrevGimbalTiltDeg_  = 0.0f;
	int32 TilePrefetchBoostFramesRemaining_ = 0;

	// Adaptive SSE — frame budget-driven Cesium LOD adjustment
	float CurrentAdaptiveSSE_      = 0.0f;   // current effective SSE (0 = not initialised)
	int32 UnderBudgetStreakFrames_ = 0;       // consecutive frames under 75% of budget

	// Output decimation — render at RenderFrameRateHz, encode at OutputFrameRateHz
	uint64 RenderFrameCounter_ = 0;

	// Phase 27D — Hot-reload config. Phase 3 moves the IFileManager::GetTimeStamp
	// syscall off-thread: PollHotReloadConfig spawns an AsyncTask that reads the
	// file mtime and flips bHotReloadFileChanged_ if it differs from the last
	// observed value. The next tick consumes the flag and runs the actual
	// load+apply on the game thread.
	float     HotReloadAccumSec_ = 0.0f;
	FDateTime LastConfigMTime_ = FDateTime::MinValue();
	TAtomic<bool> bHotReloadFileChanged_ { false };
	TAtomic<bool> bHotReloadStatInFlight_ { false };

	// Phase 22G: First-person view
	uint16 FpsEntityId_       = 0;    // 0 = FPS mode inactive
	float  FpsEyeHeightM_     = 1.7f;
	bool   bFpsFromCigi_      = false; // true = activated by CIGI ViewControl

	// Tick accounting — promoted from function-local statics so Tick() can be
	// decomposed into helpers and so other methods (CIGI log throttling) can
	// see the current tick count.
	uint64 TickCount              = 0;
	double LastHeartbeatWallSec   = 0.0;

	// Helpers
	void ApplyCigiState(float DeltaTime);
	void ApplyFpsPose();
	void ComputeGeometricLOS();
	/** Phase 27A — Update MPC scalar parameters for GPU sensor post-process material. */
	void UpdateGpuSensorMpcParams();

	/** Populate telemetry fields from the environment actor (Phase 18). */
	void ReadEnvironmentTelemetry();
	void UpdateCesiumCamera();
	void SubmitFrameToEncoder(TArray<FColor> PixelData, FCamSimTelemetry Telemetry,
	                          uint64 FrameIdx, TArray<float> DepthMetres);

	// Tick() helpers (decomposition of the former 300-line function).
	void EmitHeartbeatIfDue();
	// Returns false if the caller should abort this tick (parse failure).
	bool PollHotReloadConfig(float DeltaTime);
	void PollReadbackCompletion();
	void DispatchQueuedResultIfFree();
	// Returns true if this render frame should be skipped to honour OutputFps.
	bool ShouldSkipFrameForDecimation();
};
