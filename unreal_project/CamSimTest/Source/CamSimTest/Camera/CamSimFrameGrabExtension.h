// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "Camera/FrameGrabRequestQueue.h"
#include "Camera/ThermalAvailability.h"
#include "SensorFrameParams.h"
#include "ThermalFrameParams.h"

class FRHIGPUBufferReadback;
class FViewport;
class FSensorStatsMailbox;
struct FPostProcessMaterialInputs;
struct FScreenPassTexture;

/**
 * FCamSimFrameGrabExtension (ROADMAP 3A/3B)
 *
 * The GPU sensor graph replaces UE's tonemapper in the game view (the sensor
 * is the primary view). It writes the NV12 frame for the encoder (read back
 * into the requesting capture's slot, once per requested frame), a histogram
 * for the sensor AE (read back into a small ring and published to the
 * mailbox), and the display image the viewport shows. Scene captures
 * (Context.Viewport == null) and other viewports are ignored.
 */
class FCamSimFrameGrabExtension : public FSceneViewExtensionBase
{
public:
	/**
	 * CaptureSize: the sensor graph's output size (X % 4 == 0, even Y). Mailbox
	 * must outlive the extension's last render (the owner flushes rendering
	 * commands first).
	 */
	FCamSimFrameGrabExtension(const FAutoRegister& AutoRegister, FViewport* InGameViewport,
		FIntPoint InCaptureSize, FSensorStatsMailbox* InMailbox);
	virtual ~FCamSimFrameGrabExtension();  // out of line: FRHIGPUBufferReadback is incomplete here

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView,
		FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;

	/** Render thread: parameters for the next frames the graph runs. */
	void SetParams_RenderThread(const FSensorFrameParams& P) { Params = P; }

	/**
	 * Render thread (ROADMAP 4A): thermal parameters for the next frames; null = thermal off (luminance input). Set in
	 * the same render command as SetParams_RenderThread, so sensor and thermal parameters always belong to one tick.
	 * With parameters, ThermalCS turns the scene into in-band radiance at BeforeDOF (Task 17): it replaces scene colour
	 * ahead of the temporal upscaler, so TSR resolves the radiance with its depth/stencil-consistent jitter, and the
	 * sensor graph at ReplacingTonemapper reads the resolved radiance.
	 */
	void SetThermalParams_RenderThread(TSharedPtr<const FThermalFrameParams, ESPMode::ThreadSafe> P) { ThermalParams = MoveTemp(P); }

	/**
	 * Render thread: copy the sensor graph's NV12 output for the next
	 * game-viewport frame into Nv12Readback. GrabbedGeneration is set to
	 * R.Generation once the copy is really issued, so the poll never trusts a
	 * fence left over from an earlier cycle.
	 * With R.bInstanceIds, InstanceIdCS also runs and its buffer is copied into
	 * IdReadback; IdGrabbedGeneration is set to R.Generation only when that copy
	 * is issued (before GrabbedGeneration), so the poll knows whether to wait for it.
	 * R.bDepth does the same for the depth output (DepthReadback / DepthGrabbedGeneration); the pass runs
	 * when either is wanted.
	 */
	void PushRequest_RenderThread(const FFrameGrabRequest& R, FRHIGPUBufferReadback* Nv12Readback,
		FRHIGPUBufferReadback* IdReadback, TAtomic<uint32>* GrabbedGeneration, TAtomic<uint32>* IdGrabbedGeneration,
		FRHIGPUBufferReadback* DepthReadback = nullptr, TAtomic<uint32>* DepthGrabbedGeneration = nullptr);

	/**
	 * Any thread (ROADMAP 4A): the render thread gave thermal up for the session (ThermalCS's inputs missing at BeforeDOF,
	 * or radiance never reached the sensor graph). The capture component reads it every tick and folds it into
	 * ShouldRunThermal, so the next ticks render the visible-light proxy instead of reading luminance as radiance.
	 */
	bool AreThermalInputsMissing() const { return bThermalInputsMissing.Load(EMemoryOrder::Relaxed); }

	/** Game thread: stop matching any viewport (before the owner is destroyed). */
	void Detach_GameThread() { GameViewport.Store(nullptr); }

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	FScreenPassTexture RunSensor_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);
	/** BeforeDOF (ROADMAP 4A Task 17): ThermalCS writes radiance into a copy of scene colour (same desc and view rect). */
	FScreenPassTexture RunThermal_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);
	void ReadStats_RenderThread(FRDGBuilder& GraphBuilder, FRDGBufferRef Histogram, uint32 Serial);
	struct FTargets;
	/** Run InstanceIdCS on this view's scene textures and queue the copies the request wants: IDs into
	 *  T.IdReadback (stores Gen in T.IdGrabbed), depth into T.DepthReadback (Gen in T.DepthGrabbed). */
	void AddGroundTruthReadback_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs, const FFrameGrabRequest& Req, const FTargets& T);

	struct FTargets
	{
		FRHIGPUBufferReadback*  Nv12Readback      = nullptr;
		TAtomic<uint32>*        GrabbedGeneration = nullptr;
		FRHIGPUBufferReadback*  IdReadback        = nullptr;   // ground truth (optional)
		TAtomic<uint32>*        IdGrabbed         = nullptr;
		FRHIGPUBufferReadback*  DepthReadback     = nullptr;   // ML depth map (optional)
		TAtomic<uint32>*        DepthGrabbed      = nullptr;
	};

	TAtomic<FViewport*>    GameViewport { nullptr };
	FFrameGrabRequestQueue Requests;        // render thread
	TMap<int32, FTargets>  TargetsBySlot;   // render thread
	bool                   bWarnedViewSize = false;  // render thread
	bool                   bWarnedNoSceneTextures = false;  // render thread

	// ROADMAP 3B — GPU sensor
	const FIntPoint      CaptureSize;                          // fixed at construction
	FSensorStatsMailbox* const Mailbox;                        // fixed at construction
	FSensorFrameParams   Params;                               // render thread
	TSharedPtr<const FThermalFrameParams, ESPMode::ThreadSafe> ThermalParams;   // render thread; null = thermal off
	FThermalInputsMonitor ThermalInputsMonitor;                // render thread
	TAtomic<bool>        bThermalInputsMissing { false };      // render -> game (sticky)
	/** RunThermal_RenderThread bailed for missing inputs in this frame number (consumed by RunSensor_RenderThread). */
	bool                 bThermalInputsMissingThisFrame = false;   // render thread
	uint32               ThermalInputsMissingFrame = 0;        // render thread
	/** Set by RunThermal_RenderThread (BeforeDOF), consumed by RunSensor_RenderThread later in the same view's post
	 *  processing: scene colour at the tonemapper is then TSR-resolved radiance. Tagged with the view family's frame
	 *  number, so a flag left by a frame whose tonemapper never ran can't leak into a later frame. */
	bool                 bThermalSceneColor = false;           // render thread
	uint32               ThermalSceneColorFrame = 0;           // render thread
	/** RunSensor_RenderThread was subscribed at ReplacingTonemapper for this frame number (BeforeDOF needs it). */
	bool                 bSensorSubscribed = false;            // render thread
	uint32               SensorSubscribedFrame = 0;            // render thread
	struct FStatsSlot { TUniquePtr<FRHIGPUBufferReadback> Readback; uint32 Serial = 0; bool bPending = false; };
	static constexpr int32 NumStatsSlots = 4;
	FStatsSlot           StatsRing[NumStatsSlots];             // render thread
	int32                NextStatsSlot = 0;                    // render thread
};
