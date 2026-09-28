// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimFrameGrabExtension.h"
#include "Camera/SensorGpuTimer.h"
#include "CamSimTest.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "Sensor/SensorStatsMailbox.h"
#include "SensorGraph.h"
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

FCamSimFrameGrabExtension::~FCamSimFrameGrabExtension() = default;

bool FCamSimFrameGrabExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	FViewport* Target = GameViewport.Load();
	return Target != nullptr && Context.Viewport == Target;
}

void FCamSimFrameGrabExtension::PushRequest_RenderThread(const FFrameGrabRequest& R, FRHITexture* Target,
	FRHIGPUTextureReadback* Readback, FRHIGPUBufferReadback* Nv12Readback, TAtomic<uint32>* GrabbedGeneration)
{
	check(IsInRenderingThread());
	TargetsBySlot.Add(R.TargetIndex, { Target, Readback, Nv12Readback, GrabbedGeneration });
	Requests.Push(R);
}

void FCamSimFrameGrabExtension::PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	if (bGpuSensor.Load()) return;  // GPU sensor: the grab happens in RunSensor_RenderThread
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

// ---------------------------------------------------------------------------
// ROADMAP 3B — GPU sensor in place of the tonemapper
// ---------------------------------------------------------------------------

void FCamSimFrameGrabExtension::EnableGpuSensor_GameThread(FIntPoint InCaptureSize, FSensorStatsMailbox* InMailbox)
{
	CaptureSize = InCaptureSize;
	Mailbox = InMailbox;
	bGpuSensor.Store(true);  // SeqCst: the fields above are visible to whoever sees it
}

void FCamSimFrameGrabExtension::SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView,
	FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	if (Pass == EPostProcessingPass::ReplacingTonemapper && bIsPassEnabled && bGpuSensor.Load())
	{
		InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(this, &FCamSimFrameGrabExtension::RunSensor_RenderThread));
	}
}

void FCamSimFrameGrabExtension::ReadStats_RenderThread(FRDGBuilder& GraphBuilder, FRDGBufferRef Histogram, uint32 Serial)
{
	// Harvest finished readbacks (oldest first; the mailbox keeps the newest),
	// then queue this frame's.
	constexpr uint32 Bytes = FSensorHistogram::NumBins * sizeof(uint32);
	for (int32 K = 0; K < NumStatsSlots; ++K)
	{
		FStatsSlot& S = StatsRing[(NextStatsSlot + K) % NumStatsSlots];
		if (!S.bPending || !S.Readback->IsReady()) continue;
		if (const void* Raw = S.Readback->Lock(Bytes))  // null: do NOT Unlock
		{
			FSensorHistogram H;
			FMemory::Memcpy(H.Bins.GetData(), Raw, Bytes);
			S.Readback->Unlock();
			H.Serial = S.Serial;
			if (Mailbox) Mailbox->Publish(H);
		}
		S.bPending = false;
	}

	FStatsSlot& Slot = StatsRing[NextStatsSlot];
	if (Slot.bPending) return;  // ring full (GPU far behind): skip this frame's stats
	if (!Slot.Readback) Slot.Readback = MakeUnique<FRHIGPUBufferReadback>(TEXT("CamSimSensorStats"));
	Slot.Serial = Serial;
	Slot.bPending = true;
	NextStatsSlot = (NextStatsSlot + 1) % NumStatsSlots;

	// Inline on the render thread (as the NV12 copy below), so the fence is
	// set before the next frame's harvest looks at it.
	FRHIGPUBufferReadback* Readback = Slot.Readback.Get();
	AddReadbackBufferPass(GraphBuilder, RDG_EVENT_NAME("CamSimSensorStatsReadback"), Histogram,
		[Readback, Histogram, Bytes](FRHICommandListImmediate& RHICmdList)
	{
		Readback->EnqueueCopy(RHICmdList, Histogram->GetRHI(), Bytes);
	});
}

FScreenPassTexture FCamSimFrameGrabExtension::RunSensor_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder,
		Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
	const FScreenPassTextureSlice BloomSlice = Inputs.GetInput(EPostProcessMaterialInput::CombinedBloom);

	FSensorGraphInputs In;
	In.SceneColor = SceneColor.Texture;
	In.SceneViewRect = SceneColor.ViewRect;
	if (BloomSlice.IsValid())
	{
		const FScreenPassTexture Bloom = FScreenPassTexture::CopyFromSlice(GraphBuilder, BloomSlice);
		In.Bloom = Bloom.Texture;
		In.BloomViewRect = Bloom.ViewRect;
	}
	In.ViewUniformBuffer = View.ViewUniformBuffer.GetReference();
	In.OutputSize = CaptureSize;

	if (!bWarnedViewSize && SceneColor.ViewRect.Height() > 0 && CaptureSize.Y > 0
		&& !FMath::IsNearlyEqual(double(SceneColor.ViewRect.Width()) / SceneColor.ViewRect.Height(),
			double(CaptureSize.X) / CaptureSize.Y, 0.005))
	{
		bWarnedViewSize = true;
		UE_LOG(LogCamSim, Warning, TEXT("SensorGraph: view is %dx%d but the capture is %dx%d; the image is stretched and its vertical FOV won't match the KLV"),
			SceneColor.ViewRect.Width(), SceneColor.ViewRect.Height(), CaptureSize.X, CaptureSize.Y);
	}

	FSensorGraphOutputs Out;
	{
		// Timed by the GPU profiler; FSensorGpuTimer reads the result (see SensorGpuTimer.h).
		RDG_EVENT_SCOPE_STAT(GraphBuilder, CamSimSensor, "CamSimSensor");
		Out = AddSensorPasses(GraphBuilder, In, Params);
	}
	ReadStats_RenderThread(GraphBuilder, Out.Histogram, Params.Serial);

	FFrameGrabRequest Req;
	if (Requests.PopLatest(Req))  // this frame's request; older ones never rendered
	{
		const FTargets* T = TargetsBySlot.Find(Req.TargetIndex);
		if (T && T->Nv12Readback && T->GrabbedGeneration)
		{
			// Inline on the render thread: the generation is published only once
			// the copy is really queued (see AddReadbackTexturePass above).
			FRHIGPUBufferReadback* Readback = T->Nv12Readback;
			TAtomic<uint32>* Grabbed = T->GrabbedGeneration;
			const uint32 Gen = Req.Generation;
			const uint32 Bytes = Out.Nv12Bytes;
			const FRDGBufferRef Nv12 = Out.Nv12;
			AddReadbackBufferPass(GraphBuilder, RDG_EVENT_NAME("CamSimNv12Readback"), Nv12,
				[Readback, Nv12, Bytes, Grabbed, Gen](FRHICommandListImmediate& RHICmdList)
			{
				Readback->EnqueueCopy(RHICmdList, Nv12->GetRHI(), Bytes);
				Grabbed->Store(Gen, EMemoryOrder::SequentiallyConsistent);
			});
		}
	}

	// The viewport shows exactly what is streamed.
	FScreenPassRenderTarget Output = Inputs.OverrideOutput;
	if (!Output.IsValid())
	{
		Output = FScreenPassRenderTarget::CreateFromInput(GraphBuilder, SceneColor, ERenderTargetLoadAction::ENoAction,
			TEXT("CamSimSensorDisplay"));
	}
	AddDrawTexturePass(GraphBuilder, FScreenPassViewInfo(View), Out.SensorRgb, Output.Texture,
		FIntPoint::ZeroValue, CaptureSize, Output.ViewRect.Min, Output.ViewRect.Size());
	return FScreenPassTexture(Output);
}
