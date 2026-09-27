// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Camera/CamSimCaptureComponent.h"      // FFrameDropStats
#include "Camera/CamSimPlatformRig.h"
#include "Camera/CamSimStreamingController.h"
#include "Camera/CamSimTelemetryAssembler.h"
#include "Camera/CamSimFrameStats.h"
#include "CamSimCamera.generated.h"

class USceneCaptureComponent2D;
class UCameraComponent;
class UCesiumOriginShiftComponent;
class UCesiumGlobeAnchorComponent;
class UCamSimSubsystem;
class UCamSimGimbalComponent;
class UCamSimSensorComponent;
struct FPipelineLatencyTracker;

/**
 * ACamSimCamera
 *
 * The simulated sensor. Each tick it applies CIGI view/sensor/gimbal state,
 * keeps tile streaming on the sensor's view, and captures a frame once the
 * terrain for it has loaded. The work is split into:
 *   - FCamSimPlatformRig + UCamSimGimbalComponent — the sensor rig
 *     (platform pose, attachment, first-person view; gimbal);
 *   - UCamSimCaptureComponent — render targets, readback, sensor model, encode;
 *   - FCamSimTelemetryAssembler — the telemetry behind every KLV tag;
 *   - FCamSimStreamingController — Cesium streaming cameras, LOD, terrain gate.
 *
 * Ticks in TG_PostUpdateWork, after FCamSimEntityManager has applied the
 * frame's entity states and the platform pose. Registers itself with
 * UCamSimSubsystem.
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

	/** Capture a frame now with the current telemetry (normally done by Tick). */
	void CaptureAndEncode();

	// Phase 27B — drop stats for the health JSON
	const FFrameDropStats& GetFrameDropStats() const { return CaptureComp->GetFrameDropStats(); }
	bool IsTrackingFrameDrops() const { return CaptureComp->IsTrackingFrameDrops(); }

	/** Frames dropped because the encoder thread's queue was full. */
	uint64 GetDroppedFrameCount() const { return CaptureComp->GetDroppedFrameCount(); }
	/** Frames captured (handed to readback → sensor → encoder) so far. */
	uint64 GetFramesCaptured() const { return CaptureComp->GetFramesCaptured(); }

	/** ROADMAP 3B: sensor pipeline this session runs. */
	ESensorPipelinePath GetSensorPath() const { return CaptureComp->GetSensorPath(); }

	/**
	 * Apply this frame's host platform state (CIGI Entity Control for the
	 * camera entity). FCamSimEntityManager calls this before resolving
	 * attachments, so entities attached to the platform use this frame's pose.
	 * Runs once per frame; Tick() calls it too in case nothing else did.
	 */
	void ApplyHostPlatformState() { Platform.ApplyHostPlatformState(); }

	/** If the platform is attached to an entity, follow it. Call after the parent is placed. */
	void FollowAttachParent() { Platform.FollowAttachParent(); }

	/** False while frames are held waiting for terrain tiles to load. */
	bool IsTerrainReady() const { return Streaming.IsTerrainReady(); }

	/** Current platform geodetic pose (orientation in local NEU). */
	bool GetPlatformGeoPose(CamSimFrames::FGeoPose& OutPose) const { return Platform.GetPose(OutPose); }

	/** Snapshot of the current telemetry for external consumers (e.g. CoT sender). */
	FCamSimTelemetry GetCurrentTelemetry() const { return Telemetry.Get(); }

	/** Sensor component (for the Sensor Extended Response, opcode 107). */
	UCamSimSensorComponent* GetSensorComp() const { return SensorComp; }

	/** Phase 28G: set the pipeline latency tracker (owned by subsystem, nullable). */
	void SetLatencyTracker(FPipelineLatencyTracker* Tracker);

private:
	UPROPERTY(VisibleAnywhere, Category = "CamSim")
	TObjectPtr<USceneComponent> Root;

	/** Cesium globe anchor — places the actor on the WGS-84 globe. */
	UPROPERTY(VisibleAnywhere, Category = "CamSim")
	TObjectPtr<UCesiumGlobeAnchorComponent> GlobeAnchor;

	/** The sensor view; the gimbal rotates it relative to the platform. */
	UPROPERTY(VisibleAnywhere, Category = "CamSim")
	TObjectPtr<USceneCaptureComponent2D> SceneCapture;

	/**
	 * The player's view when render.view_source = primary (ROADMAP 3A). Child of
	 * SceneCapture with an identity transform; FOV and post-process are copied
	 * from SceneCapture every tick, which stays the source of truth until 3B.
	 */
	UPROPERTY(VisibleAnywhere, Category = "CamSim")
	TObjectPtr<UCameraComponent> SensorCamera;

	/** Rebases the georeference as the sensor travels (ROADMAP 3A); inactive when distance = 0. */
	UPROPERTY(VisibleAnywhere, Category = "CamSim")
	TObjectPtr<UCesiumOriginShiftComponent> OriginShift;

	UPROPERTY(VisibleAnywhere, Category = "CamSim")
	TObjectPtr<UCamSimGimbalComponent> GimbalComp;

	/** Sensor on/off, waveband, polarity and FOV presets. */
	UPROPERTY(VisibleAnywhere, Category = "CamSim")
	TObjectPtr<UCamSimSensorComponent> SensorComp;

	UPROPERTY(VisibleAnywhere, Category = "CamSim")
	TObjectPtr<UCamSimCaptureComponent> CaptureComp;

	UPROPERTY(Transient)
	TObjectPtr<UCamSimSubsystem> Subsystem;

	FCamSimTelemetryAssembler  Telemetry;
	FCamSimPlatformRig         Platform;
	FCamSimStreamingController Streaming;

	/** ROADMAP 3A bench stats (off unless operational.frame_stats_path is set). */
	FCamSimFrameStatsRecorder FrameStats;
	TSharedPtr<FCamSimViewFamilyCounter, ESPMode::ThreadSafe> ViewFamilyCounter;
	double LastStatsWallSec = 0.0;
	/** Set by the camera-cut check; recorded in frame stats. */
	bool bCameraCutThisFrame = false;

	FPipelineLatencyTracker* LatencyTracker = nullptr;

	// Phase 27D — config hot reload. The mtime stat runs on a background task
	// (slow storage must not block the game thread); the next tick consumes
	// bHotReloadFileChanged and reloads on the game thread.
	float     HotReloadAccumSec = 0.0f;
	FDateTime LastConfigMTime = FDateTime::MinValue();
	TAtomic<bool> bHotReloadFileChanged { false };
	TAtomic<bool> bHotReloadStatInFlight { false };

	uint64 TickCount            = 0;
	double LastHeartbeatWallSec = 0.0;

	/** CIGI View Definition, Sensor Control, View Control / Art Part → sensor state. */
	void ApplyCigiViewState(float DeltaTime);
	/** Project the DIS laser designator spot into the image (Phase 21F.2). */
	void UpdateLaserDesignator();
	/** Focus depth of field at the slant range (Phase 15E). */
	void UpdateAutoFocus();
	void EmitHeartbeatIfDue();
	/** Append this tick's render stats (ROADMAP 3A). */
	void RecordFrameStats();
	/** Primary view: mirror FOV/post-process to SensorCamera and make it the view target. */
	void ApplyPrimaryView();
	bool bViewTargetApplied = false;

	/** Reset TSR history when the view jumps (teleport, origin rebase) (ROADMAP 3A). */
	void UpdateCameraCut();
	FVector PrevViewLocCm = FVector::ZeroVector;
	FQuat   PrevViewRot   = FQuat::Identity;
	bool    bHasPrevView  = false;
	/** A parse failure keeps the current config and lets the tick carry on. */
	void PollHotReloadConfig(float DeltaTime);
};
