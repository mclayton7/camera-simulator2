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
	FRHIGPUTextureReadback* Readback)
{
	check(IsInRenderingThread());
	TargetsBySlot.Add(R.TargetIndex, { Target, Readback });
	Requests.Push(R);
}

void FCamSimFrameGrabExtension::PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	FFrameGrabRequest Req;
	if (!Requests.PopCurrent(CurrentGeneration, Req)) return;
	const FTargets* T = TargetsBySlot.Find(Req.TargetIndex);
	if (!T || !T->Target || !T->Readback || !InViewFamily.RenderTarget || InViewFamily.Views.Num() == 0) return;

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
	AddDrawTexturePass(GraphBuilder, FScreenPassViewInfo(View), Source, Dest,
		SrcRect.Min, SrcRect.Size(), FIntPoint::ZeroValue, Dest->Desc.Extent);

	AddEnqueueCopyPass(GraphBuilder, T->Readback, Dest);
	GrabCount.Store(GrabCount.Load(EMemoryOrder::Relaxed) + 1, EMemoryOrder::Relaxed);
}
