// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "Camera/FrameGrabRequestQueue.h"
#include "SensorFrameParams.h"

class FRHIGPUBufferReadback;
class FRHIGPUTextureReadback;
class FRHITexture;
class FViewport;
class FSensorStatsMailbox;
class FSensorGpuTimer;
struct FPostProcessMaterialInputs;
struct FScreenPassTexture;

/**
 * FCamSimFrameGrabExtension (ROADMAP 3A)
 *
 * When the sensor is the primary view, copies the game viewport's final image
 * into a capture-sized grab target and queues the async readback, once per
 * requested frame. Scene captures (Context.Viewport == null) and other
 * viewports are ignored.
 *
 * ROADMAP 3B: with the GPU sensor enabled, the sensor graph replaces UE's
 * tonemapper in the game view. It writes the NV12 frame for the encoder
 * (read back into the capture's slot), a histogram for the sensor AE (read
 * back into a small ring and published to the mailbox), and the display
 * image the viewport shows.
 */
class FCamSimFrameGrabExtension : public FSceneViewExtensionBase
{
public:
	FCamSimFrameGrabExtension(const FAutoRegister& AutoRegister, FViewport* InGameViewport);
	virtual ~FCamSimFrameGrabExtension();  // out of line: FRHIGPUBufferReadback is incomplete here

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;
	virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView,
		FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;

	/**
	 * Game thread, once, before any request: run the GPU sensor model in place
	 * of the tonemapper (ROADMAP 3B). Mailbox and Timer must outlive the
	 * extension's last render (the owner flushes rendering commands first).
	 */
	void EnableGpuSensor_GameThread(FIntPoint InCaptureSize, FSensorStatsMailbox* InMailbox, FSensorGpuTimer* InTimer);
	/** Render thread: parameters for the next frames the graph runs. */
	void SetParams_RenderThread(const FSensorFrameParams& P) { Params = P; }

	/**
	 * Render thread: grab the next game-viewport frame into Target and EnqueueCopy
	 * into Readback. GrabbedGeneration is set to R.Generation once the copy is
	 * really issued, so the poll never trusts a fence left over from an earlier cycle.
	 * GPU sensor: Target/Readback are null and the sensor's NV12 output is
	 * copied into Nv12Readback instead (null in the legacy path).
	 */
	void PushRequest_RenderThread(const FFrameGrabRequest& R, FRHITexture* Target, FRHIGPUTextureReadback* Readback,
		FRHIGPUBufferReadback* Nv12Readback, TAtomic<uint32>* GrabbedGeneration);

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
		FRHITexture*            Target            = nullptr;
		FRHIGPUTextureReadback* Readback          = nullptr;
		FRHIGPUBufferReadback*  Nv12Readback      = nullptr;
		TAtomic<uint32>*        GrabbedGeneration = nullptr;
	};

	TAtomic<FViewport*>    GameViewport { nullptr };
	FFrameGrabRequestQueue Requests;        // render thread
	TMap<int32, FTargets>  TargetsBySlot;   // render thread
	bool                   bWarnedViewSize = false;  // render thread

	// ROADMAP 3B — GPU sensor
	TAtomic<bool>        bGpuSensor { false };                 // writer: game (once) → readers: game/render (SeqCst)
	FIntPoint            CaptureSize = FIntPoint::ZeroValue;   // set before bGpuSensor
	FSensorStatsMailbox* Mailbox = nullptr;                    // set before bGpuSensor
	FSensorGpuTimer*     Timer   = nullptr;                    // set before bGpuSensor
	FSensorFrameParams   Params;                               // render thread
	struct FStatsSlot { TUniquePtr<FRHIGPUBufferReadback> Readback; uint32 Serial = 0; bool bPending = false; };
	static constexpr int32 NumStatsSlots = 4;
	FStatsSlot           StatsRing[NumStatsSlots];             // render thread
	int32                NextStatsSlot = 0;                    // render thread
};
