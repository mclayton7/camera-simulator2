// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "Camera/FrameGrabRequestQueue.h"

class FRHIGPUTextureReadback;
class FRHITexture;
class FViewport;

/**
 * FCamSimFrameGrabExtension (ROADMAP 3A)
 *
 * When the sensor is the primary view, copies the game viewport's final image
 * into a capture-sized grab target and queues the async readback, once per
 * requested frame. Scene captures (Context.Viewport == null) and other
 * viewports are ignored.
 */
class FCamSimFrameGrabExtension : public FSceneViewExtensionBase
{
public:
	FCamSimFrameGrabExtension(const FAutoRegister& AutoRegister, FViewport* InGameViewport);

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;

	/**
	 * Render thread: grab the next game-viewport frame into Target and EnqueueCopy
	 * into Readback. GrabbedGeneration is set to R.Generation once the copy is
	 * really issued, so the poll never trusts a fence left over from an earlier cycle.
	 */
	void PushRequest_RenderThread(const FFrameGrabRequest& R, FRHITexture* Target, FRHIGPUTextureReadback* Readback,
		TAtomic<uint32>* GrabbedGeneration);
	void SetCurrentGeneration_RenderThread(uint32 Gen) { CurrentGeneration = Gen; }

	/** Game thread: stop matching any viewport (before the owner is destroyed). */
	void Detach_GameThread() { GameViewport.Store(nullptr); }

	/** Frames grabbed so far (heartbeat / diagnostics). */
	uint64 GetGrabCount() const { return GrabCount.Load(EMemoryOrder::Relaxed); }

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	struct FTargets
	{
		FRHITexture*            Target            = nullptr;
		FRHIGPUTextureReadback* Readback          = nullptr;
		TAtomic<uint32>*        GrabbedGeneration = nullptr;
	};

	TAtomic<FViewport*>    GameViewport { nullptr };
	FFrameGrabRequestQueue Requests;        // render thread
	TMap<int32, FTargets>  TargetsBySlot;   // render thread
	uint32                 CurrentGeneration = 0;  // render thread
	TAtomic<uint64>        GrabCount { 0 };
};
