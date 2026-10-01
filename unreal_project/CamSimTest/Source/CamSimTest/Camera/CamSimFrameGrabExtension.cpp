// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimFrameGrabExtension.h"
#include "Camera/SensorGpuTimer.h"
#include "CamSimTest.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "Sensor/SensorStatsMailbox.h"
#include "SensorGraph.h"
#include "InstanceIdPass.h"
#include "ThermalPass.h"
#include "SceneTexturesConfig.h"   // FSceneTextureUniformParameters
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ScreenPass.h"
#include "SceneView.h"
#include "UnrealClient.h"

/** RDG resources of the thermal timing brackets' 1-texel copies (RunThermal_RenderThread). */
BEGIN_SHADER_PARAMETER_STRUCT(FCamSimTexelCopyParameters, )
	RDG_TEXTURE_ACCESS(Input, ERHIAccess::CopySrc)
	RDG_TEXTURE_ACCESS(Output, ERHIAccess::CopyDest)
END_SHADER_PARAMETER_STRUCT()

FCamSimFrameGrabExtension::FCamSimFrameGrabExtension(const FAutoRegister& AutoRegister, FViewport* InGameViewport,
	FIntPoint InCaptureSize, FSensorStatsMailbox* InMailbox)
	: FSceneViewExtensionBase(AutoRegister)
	, CaptureSize(InCaptureSize)
	, Mailbox(InMailbox)
{
	GameViewport.Store(InGameViewport);
}

FCamSimFrameGrabExtension::~FCamSimFrameGrabExtension() = default;

bool FCamSimFrameGrabExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	FViewport* Target = GameViewport.Load();
	return Target != nullptr && Context.Viewport == Target;
}

void FCamSimFrameGrabExtension::PushRequest_RenderThread(const FFrameGrabRequest& R,
	FRHIGPUBufferReadback* Nv12Readback, FRHIGPUBufferReadback* IdReadback, TAtomic<uint32>* GrabbedGeneration,
	TAtomic<uint32>* IdGrabbedGeneration)
{
	check(IsInRenderingThread());
	TargetsBySlot.Add(R.TargetIndex, { Nv12Readback, GrabbedGeneration, IdReadback, IdGrabbedGeneration });
	Requests.Push(R);
}

void FCamSimFrameGrabExtension::AddInstanceIdReadback_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs, const FFrameGrabRequest& Req, FRHIGPUBufferReadback* IdReadback,
	TAtomic<uint32>* IdGrabbed)
{
	const uint32 Gen = Req.Generation;
	// The deferred renderer's scene textures (depth, custom depth, custom stencil)
	// ride in the post-process inputs' scene-texture uniform buffer.
	const FSceneTextureUniformParameters* St = Inputs.SceneTextures.SceneTextures
		? Inputs.SceneTextures.SceneTextures->GetParameters().GetContents() : nullptr;
	if (!St || !St->SceneDepthTexture || !St->CustomDepthTexture || !St->CustomStencilTexture)
	{
		if (!bWarnedNoSceneTextures)
		{
			bWarnedNoSceneTextures = true;
			UE_LOG(LogCamSim, Warning, TEXT("GroundTruth: the post-process inputs carry no scene depth / custom depth / stencil; ")
				TEXT("annotations fall back to projected boxes (logged once)"));
		}
		return;
	}

	FInstanceIdInputs Ii;
	Ii.SceneDepth        = St->SceneDepthTexture;
	Ii.CustomDepth       = St->CustomDepthTexture;
	Ii.CustomStencil     = St->CustomStencilTexture;
	// The primary view's: ViewRectMin / ViewSizeAndInvSize are its render-resolution
	// rect in scene-texture texels (TSR upscales only after the depth passes).
	Ii.ViewUniformBuffer = View.ViewUniformBuffer.GetReference();
	Ii.OutputSize        = CaptureSize;
	// Submerged-hull cut (final review I2): each entity's water plane, world (doubles) -> translated world with
	// this view's pre-view translation, so only camera-relative values reach floats (LWC).
	if (Req.WaterPlanes.Num() > 0)
	{
		Ii.bWaterCut = true;
		for (FVector4f& Pl : Ii.WaterPlanes) Pl = FInstanceIdInputs::NoWaterPlane();
		for (const FEntityWaterPlane& E : Req.WaterPlanes)
		{
			if (E.Stencil == 0) continue;
			const FVector P = E.Point + View.ViewMatrices.GetPreViewTranslation();
			Ii.WaterPlanes[E.Stencil] = FVector4f(FVector3f(E.Normal), static_cast<float>(-FVector::DotProduct(E.Normal, P)));
		}
		Ii.ClipToTranslatedWorld = FMatrix44f(View.ViewMatrices.GetInvTranslatedViewProjectionMatrix());
	}
	const FRDGBufferRef Ids = AddInstanceIdPass(GraphBuilder, Ii, Params);

	const uint32 IdBytes = static_cast<uint32>(CaptureSize.X * CaptureSize.Y * 2);  // 16 bits per pixel
	AddReadbackBufferPass(GraphBuilder, RDG_EVENT_NAME("CamSimInstanceIdReadback"), Ids,
		[IdReadback, Ids, IdBytes, IdGrabbed, Gen](FRHICommandListImmediate& RHICmdList)
	{
		IdReadback->EnqueueCopy(RHICmdList, Ids->GetRHI(), IdBytes);
		IdGrabbed->Store(Gen, EMemoryOrder::SequentiallyConsistent);
	});
}

// ---------------------------------------------------------------------------
// ROADMAP 3B — GPU sensor in place of the tonemapper
// ---------------------------------------------------------------------------

void FCamSimFrameGrabExtension::SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView,
	FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	if (Pass == EPostProcessingPass::ReplacingTonemapper && bIsPassEnabled)
	{
		InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(this, &FCamSimFrameGrabExtension::RunSensor_RenderThread));
	}
	// ROADMAP 4A Task 17: the engine asks every frame, per view, on the render thread (AddPostProcessingPasses), so the
	// BeforeDOF subscription follows this frame's thermal parameters (set by a render command before the frame).
	// bIsPassEnabled is always true for BeforeDOF.
	else if (Pass == EPostProcessingPass::BeforeDOF && ThermalParams.IsValid())
	{
		InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(this, &FCamSimFrameGrabExtension::RunThermal_RenderThread));
	}
}

FScreenPassTexture FCamSimFrameGrabExtension::RunThermal_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	// The chain (AddSceneViewExtensionPassChain) carries the returned texture on as scene colour; it must keep the input's
	// view rect, which here is the render-resolution View.ViewRect, the same rect as the depth / stencil / GBuffer.
	const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder,
		Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
	bThermalSceneColor = false;
	const FSceneTextureUniformParameters* St = Inputs.SceneTextures.SceneTextures
		? Inputs.SceneTextures.SceneTextures->GetParameters().GetContents() : nullptr;
	if (!ThermalParams.IsValid() || !SceneColor.IsValid() || !St || !St->SceneDepthTexture || !St->CustomDepthTexture
		|| !St->CustomStencilTexture)
	{
		return SceneColor;   // RunSensor_RenderThread warns (visible-light proxy)
	}

	// ClipToTranslatedWorld is the view's (jittered, camera at the translated-world origin), as InstanceIdCS uses it:
	// before TSR, scene colour, depth and stencil share one render-resolution, jittered view.
	FThermalFrameParams TP = *ThermalParams;
	TP.ClipToTranslatedWorld = FMatrix44f(View.ViewMatrices.GetClipToTranslatedWorld());

	// A new texture with scene colour's desc (ThermalCS reads the scene's luminance from the old one for the fast term).
	FRDGTextureDesc Desc = SceneColor.Texture->Desc;
	Desc.Flags |= TexCreate_ShaderResource | TexCreate_UAV;
	const FRDGTextureRef Out = GraphBuilder.CreateTexture(Desc, TEXT("CamSimThermalSceneColor"));

	FThermalPassInputs Ti;
	Ti.SceneColor        = SceneColor.Texture;
	Ti.SceneColorRect    = SceneColor.ViewRect;
	Ti.SceneDepth        = St->SceneDepthTexture;
	Ti.CustomDepth       = St->CustomDepthTexture;
	Ti.CustomStencil     = St->CustomStencilTexture;
	Ti.BaseColor         = CamSimThermalPass::bBaseColorAtTonemapper ? St->GBufferCTexture : nullptr;
	Ti.ViewUniformBuffer = View.ViewUniformBuffer.GetReference();
	Ti.OutputSceneColor  = Out;
	Ti.OutputRect        = SceneColor.ViewRect;
	// Metal timing brackets (thermal_gpu_ms): MetalRHI times a GPU stat scope by the stage counters of the encoders that
	// BEGIN inside it (start of the first, end of the last), and RDG keeps consecutive compute passes in one encoder.
	// Unbracketed, ThermalCS shared its compute encoder with the post-processing passes after it (DOF, TSR): the scope read
	// ~7.3 ms at any resolution while the frame's GPU time didn't change; with a compute pass opened just before the scope,
	// it read 0 (no encoder began inside it). A never-culled 1-texel blit before the scope and one inside it, after the
	// dispatch, give ThermalCS its own compute encoder (1080p: 0.134 ms median, the closing blit included). Two 1-texel
	// copies per IR frame; harmless on RHIs with per-scope timestamps.
	auto AddTexelCopy = [&GraphBuilder](FRDGTextureRef Src, FIntPoint At, const TCHAR* Name)
	{
		const FRDGTextureRef Dst = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint(1, 1), Src->Desc.Format,
			FClearValueBinding::None, TexCreate_ShaderResource), Name);
		FRHICopyTextureInfo Ci;
		Ci.Size = FIntVector(1, 1, 1);
		Ci.SourcePosition = FIntVector(At.X, At.Y, 0);
		FCamSimTexelCopyParameters* Pp = GraphBuilder.AllocParameters<FCamSimTexelCopyParameters>();
		Pp->Input = Src;
		Pp->Output = Dst;
		// NeverCull: nothing reads Dst, and RDG culls a pass whose outputs are unused (AddCopyTexturePass would vanish).
		GraphBuilder.AddPass(RDG_EVENT_NAME("%s", Name), Pp, ERDGPassFlags::Copy | ERDGPassFlags::NeverCull,
			[Src, Dst, Ci](FRDGAsyncTask, FRHICommandList& RHICmdList) { RHICmdList.CopyTexture(Src->GetRHI(), Dst->GetRHI(), Ci); });
	};
	AddTexelCopy(SceneColor.Texture, SceneColor.ViewRect.Min, TEXT("CamSimThermalTimingBegin"));
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, CamSimThermal, "CamSimThermal");
		AddThermalPass(GraphBuilder, Ti, TP);
		AddTexelCopy(Out, SceneColor.ViewRect.Min, TEXT("CamSimThermalTimingEnd"));
	}
	bThermalSceneColor = true;
	ThermalSceneColorFrame = View.Family ? View.Family->FrameNumber : 0;
	return FScreenPassTexture(Out, SceneColor.ViewRect);
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

	// ROADMAP 4A Task 17: thermal IR — ThermalCS ran at BeforeDOF this frame, so scene colour is the TSR-resolved in-band
	// radiance (raw W m^-2 sr^-1: no pre-exposure, no bloom; Params.InputScale = 1 / B(300 K) set by the game thread).
	const bool bRadiance = bThermalSceneColor && ThermalParams.IsValid()
		&& ThermalSceneColorFrame == (View.Family ? View.Family->FrameNumber : 0);
	bThermalSceneColor = false;
	if (bRadiance)
	{
		In.Bloom          = nullptr;
		In.BloomViewRect  = FIntRect();
		In.bRadianceInput = true;
	}
	else if (ThermalParams.IsValid() && !bWarnedThermalInputs)
	{
		bWarnedThermalInputs = true;
		UE_LOG(LogCamSim, Warning, TEXT("Thermal: the post-process inputs carry no depth / custom depth / stencil at BeforeDOF; ")
			TEXT("IR falls back to the visible-light proxy this session (logged once)"));
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
			// Ground truth (ROADMAP 2.7): the instance-ID copy is queued BEFORE the
			// NV12 one, whose lambda publishes GrabbedGeneration — so once the poll
			// sees the generation, IdGrabbed already says whether an ID copy exists.
			if (Req.bInstanceIds && T->IdReadback && T->IdGrabbed)
			{
				AddInstanceIdReadback_RenderThread(GraphBuilder, View, Inputs, Req, T->IdReadback, T->IdGrabbed);
			}
			// Inline on the render thread: the generation is published only once
			// the copy is really queued (the AddReadbackBufferPass lambda below
			// stores it right after EnqueueCopy).
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
