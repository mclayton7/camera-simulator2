// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "Camera/FrameGrabRequestQueue.h"
#include "SensorFrameParams.h"

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
	 * Render thread: copy the sensor graph's NV12 output for the next
	 * game-viewport frame into Nv12Readback. GrabbedGeneration is set to
	 * R.Generation once the copy is really issued, so the poll never trusts a
	 * fence left over from an earlier cycle.
	 */
	void PushRequest_RenderThread(const FFrameGrabRequest& R, FRHIGPUBufferReadback* Nv12Readback,
		TAtomic<uint32>* GrabbedGeneration);

	/** Game thread: stop matching any viewport (before the owner is destroyed). */
	void Detach_GameThread() { GameViewport.Store(nullptr); }

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	FScreenPassTexture RunSensor_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);
	void ReadStats_RenderThread(FRDGBuilder& GraphBuilder, FRDGBufferRef Histogram, uint32 Serial);

	struct FTargets
	{
		FRHIGPUBufferReadback*  Nv12Readback      = nullptr;
		TAtomic<uint32>*        GrabbedGeneration = nullptr;
	};

	TAtomic<FViewport*>    GameViewport { nullptr };
	FFrameGrabRequestQueue Requests;        // render thread
	TMap<int32, FTargets>  TargetsBySlot;   // render thread
	bool                   bWarnedViewSize = false;  // render thread

	// ROADMAP 3B — GPU sensor
	const FIntPoint      CaptureSize;                          // fixed at construction
	FSensorStatsMailbox* const Mailbox;                        // fixed at construction
	FSensorFrameParams   Params;                               // render thread
	struct FStatsSlot { TUniquePtr<FRHIGPUBufferReadback> Readback; uint32 Serial = 0; bool bPending = false; };
	static constexpr int32 NumStatsSlots = 4;
	FStatsSlot           StatsRing[NumStatsSlots];             // render thread
	int32                NextStatsSlot = 0;                    // render thread
};
