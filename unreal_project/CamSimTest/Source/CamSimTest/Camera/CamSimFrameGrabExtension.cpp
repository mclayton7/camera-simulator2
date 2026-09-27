// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimFrameGrabExtension.h"
#include "CamSimTest.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ScreenPass.h"
#include "SceneView.h"
#include "UnrealClient.h"

FCamSimFrameGrabExtension::FCamSimFrameGrabExtension(const FAutoRegister& AutoRegister, FViewport* InGameViewport)
	: FSceneViewExtensionBase(AutoRegister)
{
	GameViewport.Store(InGameViewport);
}

bool FCamSimFrameGrabExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	FViewport* Target = GameViewport.Load();
	return Target != nullptr && Context.Viewport == Target;
}

void FCamSimFrameGrabExtension::PushRequest_RenderThread(const FFrameGrabRequest& R, FRHITexture* Target,
	FRHIGPUTextureReadback* Readback, TAtomic<uint32>* GrabbedGeneration)
{
	check(IsInRenderingThread());
	TargetsBySlot.Add(R.TargetIndex, { Target, Readback, GrabbedGeneration });
	Requests.Push(R);
}

void FCamSimFrameGrabExtension::PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	FFrameGrabRequest Req;
	if (!Requests.PopLatest(Req)) return;  // this frame's request; older ones never rendered
	const FTargets* T = TargetsBySlot.Find(Req.TargetIndex);
	if (!T || !T->Target || !T->Readback || !T->GrabbedGeneration || !InViewFamily.RenderTarget || InViewFamily.Views.Num() == 0) return;

	FRDGTextureRef Source = InViewFamily.RenderTarget->GetRenderTargetTexture(GraphBuilder);
	if (!Source)
	{
		UE_LOG(LogCamSim, Warning, TEXT("FrameGrab: game viewport has no render target texture — frame %llu not grabbed"),
			static_cast<unsigned long long>(Req.FrameIndex));
		return;
	}
	FRDGTextureRef Dest = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(T->Target, TEXT("CamSimGrabTarget")));

	// The view rect, not the whole backbuffer: the viewport may be larger than
	// the view (DPI scale, window chrome). The draw scales to the grab target.
	const FSceneView& View = *InViewFamily.Views[0];
	const FIntRect SrcRect = View.UnscaledViewRect;
	// A uniform scale (Retina 2x) is harmless; a different aspect (e.g. a window
	// that doesn't fit on screen) stretches the image, so its vertical FOV no
	// longer matches the KLV.
	const FIntPoint DestSize = Dest->Desc.Extent;
	if (!bWarnedViewSize && SrcRect.Height() > 0 && DestSize.Y > 0
		&& !FMath::IsNearlyEqual(double(SrcRect.Width()) / SrcRect.Height(), double(DestSize.X) / DestSize.Y, 0.005))
	{
		bWarnedViewSize = true;
		UE_LOG(LogCamSim, Warning, TEXT("FrameGrab: view is %dx%d but the capture is %dx%d; the image is stretched and its vertical FOV won't match the KLV"),
			SrcRect.Width(), SrcRect.Height(), DestSize.X, DestSize.Y);
	}
	AddDrawTexturePass(GraphBuilder, FScreenPassViewInfo(View), Source, Dest,
		SrcRect.Min, SrcRect.Size(), FIntPoint::ZeroValue, Dest->Desc.Extent);

	// Inline on the render thread (AddEnqueueCopyPass runs as an async RDG task,
	// which could clear the fence after the next poll already read it). The
	// generation is published only once the copy is really queued.
	FRHIGPUTextureReadback* Readback = T->Readback;
	TAtomic<uint32>* Grabbed = T->GrabbedGeneration;
	const uint32 Gen = Req.Generation;
	AddReadbackTexturePass(GraphBuilder, RDG_EVENT_NAME("CamSimGrabReadback"), Dest,
		[Readback, Dest, Grabbed, Gen](FRHICommandListImmediate& RHICmdList)
	{
		Readback->EnqueueCopy(RHICmdList, Dest->GetRHI());
		Grabbed->Store(Gen, EMemoryOrder::SequentiallyConsistent);
	});
}
