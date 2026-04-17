// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/ThreadSafeBool.h"
#include "Metadata/KlvBuilder.h"
#include "Sensor/SensorTypes.h"      // ESensorMode
#include "Sensor/IPixelPipeline.h"   // IPixelPipeline
#include "Encoder/EncoderThread.h"   // FEncoderThread — must be complete for TUniquePtr in UHT .gen.cpp
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

/** Phase 27B — per-category frame drop counters. */
struct FFrameDropStats
{
	TAtomic<int32> EncoderBusy     { 0 };
	TAtomic<int32> ReadbackTimeout { 0 };
	TAtomic<int32> SocketError     { 0 };
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

	/**
	 * Apply Cesium tileset streaming parameters (SSE, cache, culling, physics)
	 * to every ACesium3DTileset in the world. Called from BeginPlay() once and
	 * again from UCamSimSubsystem::HotReloadConfig() so runtime tuning takes
	 * effect without a level reload. Safe to call with a null World (no-op).
	 */
	static void ApplyCesiumTilesetTuning(UWorld* World, const struct FCamSimConfig& Cfg);

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

	/**
	 * Set to true while the sensor post-process task is running on the previous frame.
	 * Cleared by the sensor async task after depositing into the encoder queue.
	 * (Replaces the old bEncoderBusy — encode now runs on a separate persistent thread.)
	 */
	FThreadSafeBool bSensorBusy;

	/** Persistent encoder thread — drains processed frames from SPSC queue. */
	TUniquePtr<FEncoderThread> EncoderThread;

	/**
	 * Set to true between CaptureAndEncode() and game-thread readback completion.
	 * Cleared on game thread once pixels have been copied.
	 */
	bool bReadbackPending = false;

	/**
	 * Pool of GPU→CPU readback helpers — one per RenderTarget so the EnqueueCopy
	 * triggered on frame N+1 can't race the Lock/Unlock that still targets
	 * frame N's staging copy. Indexed by PendingReadbackTargetIndex.
	 */
	TArray<TUniquePtr<FRHIGPUTextureReadback>> ColorReadbackPool;

	/** Set by render thread after EnqueueCopy; game thread polls IsReady() only after this is true. */
	TAtomic<bool> bReadbackDMAIssued{false};

	/**
	 * Async readback result hand-off (render thread writes → game thread reads).
	 * Render command fills AsyncPixels_/AsyncDepth_, then sets bPollComplete_
	 * with sequentially-consistent semantics; the matching acquire on the game
	 * thread makes the array contents visible without FlushRenderingCommands().
	 */
	TArray<FColor> AsyncPixels_;
	TArray<float>  AsyncDepth_;
	TAtomic<bool>  bPollComplete_{false};  // data ready for consume
	TAtomic<bool>  bPollFailed_{false};    // Lock failure / bad format
	/**
	 * Monotonic poll generation — game thread bumps on every new CaptureAndEncode.
	 * Each poll render command captures the generation by value and no-ops if
	 * the generation has advanced, preventing stale polls from one frame from
	 * resurrecting an already-consumed result once we advance to the next frame.
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

	/** Monotonically increasing count of frames dropped due to encoder busy. */
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

	// Phase 27D — Hot-reload config
	float     HotReloadAccumSec_ = 0.0f;
	FDateTime LastConfigMTime_ = FDateTime::MinValue();

	// Phase 22G: First-person view
	uint16 FpsEntityId_       = 0;    // 0 = FPS mode inactive
	float  FpsEyeHeightM_     = 1.7f;
	bool   bFpsFromCigi_      = false; // true = activated by CIGI ViewControl

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
};
