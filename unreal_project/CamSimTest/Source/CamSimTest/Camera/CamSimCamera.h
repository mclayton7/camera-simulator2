// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/ThreadSafeBool.h"
#include "Metadata/KlvBuilder.h"
#include "Sensor/SensorTypes.h"      // ESensorMode
#include "Sensor/IPixelPipeline.h"   // IPixelPipeline
#include "CamSimCamera.generated.h"

class USceneCaptureComponent2D;
class UCesiumGlobeAnchorComponent;
class UCamSimSubsystem;
class UCamSimGimbalComponent;
class UCamSimSensorComponent;
class FRHIGPUTextureReadback;  // forward-declare async readback helper

/**
 * ACamSimCamera
 *
 * A world-partition-friendly actor that:
 *   1. Receives geospatial pose updates from the CIGI receiver (via Tick).
 *   2. Triggers a SceneCapture2D render.
 *   3. Reads back BGRA pixels from the GPU and dispatches them to the
 *      FVideoEncoder running on a background task thread.
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

	/** Render target shared between SceneCapture and the GPU readback. */
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> RenderTarget;

	/** Cached pointer to our subsystem (set in BeginPlay). */
	UPROPERTY(Transient)
	TObjectPtr<UCamSimSubsystem> Subsystem;

	/** Frame counter for PTS calculation. */
	uint64 FrameIndex = 0;

	/**
	 * Set to true while the encoder task is processing the previous frame.
	 * Cleared by the background encode task when it finishes.
	 */
	FThreadSafeBool bEncoderBusy;

	/**
	 * Set to true between EnqueueCopy (State A) and IsReady+Lock (State B).
	 * Cleared from the render thread once pixels have been copied.
	 */
	FThreadSafeBool bReadbackPending;

	/** Non-blocking GPU-to-CPU texture readback helper (async DMA). */
	FRHIGPUTextureReadback* GPUReadback = nullptr;

	/**
	 * Set to true by the first CamSimPollReadback render command that successfully
	 * claims the completed readback.  Reset to false by CamSimEnqueueReadback at
	 * the start of each new capture session.
	 *
	 * RENDER THREAD ONLY — no atomic needed; all accesses are inside ENQUEUE_RENDER_COMMAND.
	 */
	bool bReadbackClaimed = false;

	/**
	 * RENDER THREAD ONLY — requires IsReady() to be true in two consecutive poll
	 * commands before Lock(). This mitigates occasional early-ready signaling on
	 * some Vulkan drivers that can present as bottom-half tearing.
	 */
	bool bReadbackReadySeen = false;

	/** RENDER THREAD ONLY — current readback session ID for stale poll suppression. */
	uint64 ReadbackSessionIdRT = 0;

	/** Frame index in-flight through the GPU readback pipeline. */
	uint64 PendingFrameIndex = 0;

	/** Monotonic session counter for each EnqueueCopy issued on the game thread. */
	uint64 ReadbackSessionCounter = 0;

	/** Current readback session ID captured by poll commands (game thread only). */
	uint64 ActiveReadbackSessionId = 0;

	/** Telemetry snapshot captured at the same time as the in-flight frame. */
	FCamSimTelemetry PendingTelemetry;

	/** Monotonically increasing count of frames dropped due to encoder busy. */
	TAtomic<uint64> DroppedFrameCount { 0 };

	/** Telemetry cached from the last applied CIGI state. */
	FCamSimTelemetry CurrentTelemetry;

	/** Cesium camera manager registration ID (-1 = not registered). */
	int32 CesiumCameraId = -1;

	// Gimbal and sensor state have been extracted to UCamSimGimbalComponent
	// and UCamSimSensorComponent. Access via GimbalComp / SensorComp.

	/** CPU-side sensor post-processing pipeline (Phase 11). */
	TUniquePtr<IPixelPipeline> SensorFX;

	// Helpers
	void ApplyCigiState(float DeltaTime);
	void ComputeGeometricLOS();
	void UpdateCesiumCamera();
	void SubmitFrameToEncoder(TArray<FColor> PixelData, FCamSimTelemetry Telemetry, uint64 FrameIdx);
};
