// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "SensorGraph.h"
#include "Sensor/SensorReference.h"

namespace
{
	struct FGpuResult { TArray<uint8> Nv12; FSensorHistogram Histogram; bool bOk = false; };

	/** Upload Scene (W*H float RGBA), run AddSensorPasses, read back NV12 + histogram synchronously. */
	FGpuResult RunOnGpu(const TArray<FLinearColor>& Scene, int32 SrcW, int32 SrcH, FIntPoint OutSize, const FSensorFrameParams& P)
	{
		FGpuResult Result;
		ENQUEUE_RENDER_COMMAND(CamSimSensorGpuTest)([&](FRHICommandListImmediate& RHICmdList)
		{
			const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("CamSimTestScene"), SrcW, SrcH, PF_A32B32G32R32F)
				.SetFlags(ETextureCreateFlags::ShaderResource).SetInitialState(ERHIAccess::SRVMask);
			FTextureRHIRef Tex = RHICmdList.CreateTexture(Desc);
			RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, SrcW, SrcH), SrcW * sizeof(FLinearColor),
				reinterpret_cast<const uint8*>(Scene.GetData()));

			FRHIGPUBufferReadback Nv12Rb(TEXT("CamSimTestNv12"));
			FRHIGPUBufferReadback HistRb(TEXT("CamSimTestHist"));
			uint32 Nv12Bytes = 0;
			{
				FRDGBuilder GraphBuilder(RHICmdList);
				FSensorGraphInputs In;
				In.SceneColor = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Tex, TEXT("CamSimTestScene")));
				In.SceneViewRect = FIntRect(0, 0, SrcW, SrcH);
				In.OutputSize = OutSize;
				const FSensorGraphOutputs Out = AddSensorPasses(GraphBuilder, In, P);
				Nv12Bytes = Out.Nv12Bytes;
				AddEnqueueCopyPass(GraphBuilder, &Nv12Rb, Out.Nv12, Nv12Bytes);
				AddEnqueueCopyPass(GraphBuilder, &HistRb, Out.Histogram, FSensorHistogram::NumBins * sizeof(uint32));
				GraphBuilder.Execute();
			}
			RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (!Nv12Rb.IsReady() || !HistRb.IsReady()) return;
			Result.Nv12.SetNumUninitialized(Nv12Bytes);
			FMemory::Memcpy(Result.Nv12.GetData(), Nv12Rb.Lock(Nv12Bytes), Nv12Bytes);
			Nv12Rb.Unlock();
			FMemory::Memcpy(Result.Histogram.Bins.GetData(), HistRb.Lock(FSensorHistogram::NumBins * sizeof(uint32)),
				FSensorHistogram::NumBins * sizeof(uint32));
			HistRb.Unlock();
			Result.bOk = true;
		});
		FlushRenderingCommands();
		return Result;
	}

	void Compare(FAutomationTestBase& T, const FGpuResult& G, const CamSimSensorRef::FResult& R, int32 W, int32 H, bool bCheckHistogram = true)
	{
		if (!T.TestTrue(TEXT("GPU readback"), G.bOk && G.Nv12.Num() == R.Nv12.Num())) return;
		int32 MaxY = 0, MaxC = 0;
		for (int32 I = 0; I < W * H; ++I) MaxY = FMath::Max(MaxY, FMath::Abs((int32)G.Nv12[I] - (int32)R.Nv12[I]));
		for (int32 I = W * H; I < R.Nv12.Num(); ++I) MaxC = FMath::Max(MaxC, FMath::Abs((int32)G.Nv12[I] - (int32)R.Nv12[I]));
		T.AddInfo(FString::Printf(TEXT("GPU vs reference: max |dY| %d DN, max |dUV| %d DN"), MaxY, MaxC));
		T.TestTrue(FString::Printf(TEXT("Y within 1 DN (max %d)"), MaxY), MaxY <= 1);
		T.TestTrue(FString::Printf(TEXT("UV within 2 DN (max %d)"), MaxC), MaxC <= 2);
		if (bCheckHistogram)
		{
			for (int32 B = 0; B < FSensorHistogram::NumBins; ++B)
			{
				if (G.Histogram.Bins[B] != R.Histogram.Bins[B])
				{
					T.AddError(FString::Printf(TEXT("histogram bin %d: GPU %u, reference %u"), B, G.Histogram.Bins[B], R.Histogram.Bins[B]));
					break;
				}
			}
		}
	}

	/** Grey pixels at histogram bin centres covering all 256 bins (a 32-stop log gradient). */
	TArray<FLinearColor> LogGradient(int32 W, int32 H)
	{
		TArray<FLinearColor> A; A.SetNumUninitialized(W * H);
		for (int32 I = 0; I < W * H; ++I)
		{
			const float V = FMath::Exp2(FSensorHistogram::BinCentreLog2(I % FSensorHistogram::NumBins));
			A[I] = FLinearColor(V, V, V, 1);
		}
		return A;
	}

	bool SkipWithoutGpu(FAutomationTestBase& T)
	{
		if (GUsingNullRHI) { T.AddInfo(TEXT("skipped: NullRHI (run scripts/run_gpu_tests.sh)")); return true; }
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuEoTest, "CamSim.GPU.Sensor.EoLogGradient",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuEoTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 64, H = 32;
	const TArray<FLinearColor> Scene = LogGradient(W, H);
	FSensorFrameParams P; P.PhotonGain = FMath::Exp2(-4.0f);
	Compare(*this, RunOnGpu(Scene, W, H, FIntPoint(W, H), P), CamSimSensorRef::Run(Scene, W, H, P), W, H);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuIrTest, "CamSim.GPU.Sensor.IrStepEdgeBlackHot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuIrTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 64, H = 32;
	TArray<FLinearColor> Scene; Scene.SetNumUninitialized(W * H);
	for (int32 I = 0; I < W * H; ++I) { const float V = (I % W) < W / 2 ? 0.5f : 8.0f; Scene[I] = FLinearColor(V, V, V, 1); }
	FSensorFrameParams P; P.Mode = ESensorGraphMode::IR; P.PhotonGain = 0.05f; P.DisplayGain = 2.0f; P.DisplayOffset = 0.05f; P.bBlackHot = 1;
	Compare(*this, RunOnGpu(Scene, W, H, FIntPoint(W, H), P), CamSimSensorRef::Run(Scene, W, H, P), W, H);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuSanitizeTest, "CamSim.GPU.Sensor.SanitizesNaNInfNegative",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuSanitizeTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 64, H = 32;
	TArray<FLinearColor> Scene = LogGradient(W, H);
	for (int32 I = 0; I < W * H; I += 7) Scene[I] = FLinearColor(NAN, NAN, NAN, 1);
	for (int32 I = 3; I < W * H; I += 11) Scene[I] = FLinearColor(INFINITY, INFINITY, INFINITY, 1);
	for (int32 I = 5; I < W * H; I += 13) Scene[I] = FLinearColor(-1, -1, -1, 1);
	FSensorFrameParams P; P.PhotonGain = FMath::Exp2(-4.0f);
	Compare(*this, RunOnGpu(Scene, W, H, FIntPoint(W, H), P), CamSimSensorRef::Run(Scene, W, H, P), W, H);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorGpuScaledTest, "CamSim.GPU.Sensor.GpuMatchesReferenceScaled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorGpuScaledTest::RunTest(const FString& Parameters)
{
	// View 2x the capture (Retina): the graph resamples bilinearly. A 2x2-constant
	// scene makes bilinear at the 2x2 centre exact, so the reference is the 1x image.
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 W = 32, H = 16;
	const TArray<FLinearColor> Small = LogGradient(W, H);
	TArray<FLinearColor> Big; Big.SetNumUninitialized(4 * W * H);
	for (int32 Y = 0; Y < 2 * H; ++Y) for (int32 X = 0; X < 2 * W; ++X) Big[Y * 2 * W + X] = Small[(Y / 2) * W + X / 2];
	FSensorFrameParams P; P.PhotonGain = FMath::Exp2(-4.0f);
	Compare(*this, RunOnGpu(Big, 2 * W, 2 * H, FIntPoint(W, H), P), CamSimSensorRef::Run(Small, W, H, P), W, H);
	return true;
}
