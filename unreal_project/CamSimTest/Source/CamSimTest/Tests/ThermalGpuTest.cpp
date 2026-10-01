// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ThermalPass.h"
#include "Tests/ThermalTestScene.h"

// CamSim.GPU.Thermal.*: ThermalCS (CamSimThermal.usf) against CamSimThermalRef on synthetic textures — sky, entity
// visible/occluded/over water, water, terrain, shadow, NaN/Inf — within 1e-4 relative radiance (ROADMAP 4A).

namespace
{
	template <typename T>
	TArray<T> Embed(const TArray<T>& Src, int32 W, int32 H, FIntPoint Extent, FIntPoint Min, const T& Fill)
	{
		TArray<T> Out;
		Out.Init(Fill, Extent.X * Extent.Y);
		for (int32 Y = 0; Y < H; ++Y)
			for (int32 X = 0; X < W; ++X) Out[(Min.Y + Y) * Extent.X + Min.X + X] = Src[Y * W + X];
		return Out;
	}

	FTextureRHIRef UploadRgba(FRHICommandListImmediate& RHICmdList, const TArray<FLinearColor>& Texels, FIntPoint Ext, const TCHAR* Name)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(Name, Ext.X, Ext.Y, PF_A32B32G32R32F)
			.SetFlags(ETextureCreateFlags::ShaderResource).SetInitialState(ERHIAccess::SRVMask);
		FTextureRHIRef Tex = RHICmdList.CreateTexture(Desc);
		RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, Ext.X, Ext.Y), Ext.X * sizeof(FLinearColor),
			reinterpret_cast<const uint8*>(Texels.GetData()));
		return Tex;
	}

	FTextureRHIRef UploadFloat(FRHICommandListImmediate& RHICmdList, const TArray<float>& Texels, FIntPoint Ext, const TCHAR* Name)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(Name, Ext.X, Ext.Y, PF_R32_FLOAT)
			.SetFlags(ETextureCreateFlags::ShaderResource).SetInitialState(ERHIAccess::SRVMask);
		FTextureRHIRef Tex = RHICmdList.CreateTexture(Desc);
		RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, Ext.X, Ext.Y), Ext.X * sizeof(float),
			reinterpret_cast<const uint8*>(Texels.GetData()));
		return Tex;
	}

	/** PF_R8G8B8A8_UINT with the value in every channel, so STENCIL_COMPONENT_SWIZZLE reads it whichever channel it names. */
	FTextureRHIRef UploadStencil(FRHICommandListImmediate& RHICmdList, const TArray<uint8>& Ids, FIntPoint Ext)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("CamSimTestThermalStencil"), Ext.X, Ext.Y, PF_R8G8B8A8_UINT)
			.SetFlags(ETextureCreateFlags::ShaderResource).SetInitialState(ERHIAccess::SRVMask);
		FTextureRHIRef Tex = RHICmdList.CreateTexture(Desc);
		TArray<uint8> Rgba;
		Rgba.SetNumUninitialized(Ids.Num() * 4);
		for (int32 I = 0; I < Ids.Num(); ++I) { for (int32 C = 0; C < 4; ++C) Rgba[I * 4 + C] = Ids[I]; }
		RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, Ext.X, Ext.Y), Ext.X * 4, Rgba.GetData());
		return Tex;
	}

	/** GBufferC's real format (Task 1): PF_B8G8R8A8 + TexCreate_SRGB, grey bytes. Sampling its SRV decodes to linear. */
	FTextureRHIRef UploadSrgbBase(FRHICommandListImmediate& RHICmdList, const TArray<uint8>& Grey, FIntPoint Ext)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("CamSimTestThermalBaseSrgb"), Ext.X, Ext.Y, PF_B8G8R8A8)
			.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SRGB).SetInitialState(ERHIAccess::SRVMask);
		FTextureRHIRef Tex = RHICmdList.CreateTexture(Desc);
		TArray<uint8> Bgra;
		Bgra.SetNumUninitialized(Grey.Num() * 4);
		for (int32 I = 0; I < Grey.Num(); ++I) { Bgra[I * 4 + 0] = Bgra[I * 4 + 1] = Bgra[I * 4 + 2] = Grey[I]; Bgra[I * 4 + 3] = 255; }
		RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, Ext.X, Ext.Y), Ext.X * 4, Bgra.GetData());
		return Tex;
	}

	/** sRGB byte of a linear grey (the encoder's rounding: floor(x + 0.5)). */
	uint8 LinearToSrgbByte(float L)
	{
		const float S = L <= 0.0031308f ? L * 12.92f : 1.055f * FMath::Pow(L, 1.0f / 2.4f) - 0.055f;
		return static_cast<uint8>(FMath::Clamp(FMath::FloorToInt32(S * 255.0f + 0.5f), 0, 255));
	}

	struct FLayout { FIntPoint ColorExtent, ColorMin, DepthExtent, DepthMin; };

	enum class EBase : uint8 { None, Float, HardwareSrgb };

	/** Upload the scene into textures laid out per L (view rects offset inside larger textures), run AddThermalPass, read back.
	 *  HardwareSrgb: base colour as the bytes of BaseBytes (DW x DH) in an sRGB-flagged 8-bit texture. */
	TArray<float> RunThermalOnGpu(const CamSimThermalTest::FThermalTestScene& S, const FLayout& L, EBase BaseMode, const TArray<uint8>& BaseBytes)
	{
		const TArray<FLinearColor> Color = Embed(S.Color, S.W, S.H, L.ColorExtent, L.ColorMin, FLinearColor::Black);
		const TArray<float> Depth = Embed(S.Depth, S.DW, S.DH, L.DepthExtent, L.DepthMin, 0.0f);
		const TArray<float> Custom = Embed(S.Custom, S.DW, S.DH, L.DepthExtent, L.DepthMin, 0.0f);
		const TArray<uint8> Stencil = Embed(S.Stencil, S.DW, S.DH, L.DepthExtent, L.DepthMin, static_cast<uint8>(0));
		const TArray<FLinearColor> Base = Embed(S.Base, S.DW, S.DH, L.DepthExtent, L.DepthMin, FLinearColor::Black);
		const TArray<uint8> Bytes = BaseMode == EBase::HardwareSrgb
			? Embed(BaseBytes, S.DW, S.DH, L.DepthExtent, L.DepthMin, static_cast<uint8>(0)) : TArray<uint8>();
		TArray<float> Result;
		ENQUEUE_RENDER_COMMAND(CamSimThermalGpuTest)([&](FRHICommandListImmediate& RHICmdList)
		{
			FTextureRHIRef ColorTex   = UploadRgba(RHICmdList, Color, L.ColorExtent, TEXT("CamSimTestThermalColor"));
			FTextureRHIRef DepthTex   = UploadFloat(RHICmdList, Depth, L.DepthExtent, TEXT("CamSimTestThermalDepth"));
			FTextureRHIRef CustomTex  = UploadFloat(RHICmdList, Custom, L.DepthExtent, TEXT("CamSimTestThermalCustom"));
			FTextureRHIRef StencilTex = UploadStencil(RHICmdList, Stencil, L.DepthExtent);
			FTextureRHIRef BaseTex    = BaseMode == EBase::HardwareSrgb ? UploadSrgbBase(RHICmdList, Bytes, L.DepthExtent)
				: UploadRgba(RHICmdList, Base, L.DepthExtent, TEXT("CamSimTestThermalBase"));
			FRHIGPUTextureReadback Rb(TEXT("CamSimTestThermalRadiance"));
			{
				FRDGBuilder GraphBuilder(RHICmdList);
				FThermalPassInputs In;
				In.SceneColor     = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(ColorTex, TEXT("CamSimTestThermalColor")));
				In.SceneColorRect = FIntRect(L.ColorMin, L.ColorMin + FIntPoint(S.W, S.H));
				In.SceneDepth     = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(DepthTex, TEXT("CamSimTestThermalDepth")));
				In.CustomDepth    = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(CustomTex, TEXT("CamSimTestThermalCustom")));
				const FRDGTextureRef StencilRdg = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(StencilTex, TEXT("CamSimTestThermalStencil")));
				In.CustomStencil  = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(StencilRdg));
				In.BaseColor      = BaseMode != EBase::None
					? GraphBuilder.RegisterExternalTexture(CreateRenderTarget(BaseTex, TEXT("CamSimTestThermalBase"))) : nullptr;
				In.DepthViewRect  = FIntRect(L.DepthMin, L.DepthMin + FIntPoint(S.DW, S.DH));
				const FRDGTextureRef Radiance = AddThermalPass(GraphBuilder, In, S.P);
				AddEnqueueCopyPass(GraphBuilder, &Rb, Radiance);
				GraphBuilder.Execute();
			}
			RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (!Rb.IsReady()) return;
			int32 Pitch = 0;
			const float* Data = static_cast<const float*>(Rb.Lock(Pitch));
			Result.SetNumUninitialized(S.W * S.H);
			for (int32 Y = 0; Y < S.H; ++Y) FMemory::Memcpy(&Result[Y * S.W], Data + static_cast<int64>(Y) * Pitch, S.W * sizeof(float));
			Rb.Unlock();
		});
		FlushRenderingCommands();
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalGpuMatchesCpuTest, "CamSim.GPU.Thermal.MatchesCpu",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalGpuMatchesCpuTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("skipped: NullRHI (run scripts/run_gpu_tests.sh)")); return true; }
	// Mirror tolerance: ThermalCS vs CamSimThermalRef on the same inputs (never loosen it).
	constexpr float MirrorTol = 1e-4f;
	// The hardware sRGB decode is not bit-exact with CamSimThermalRef::SrgbToLinear (M1 Pro/Metal: 3.0e-4 relative radiance
	// at the worst entity pixel, where the fast term's 1/BaseLum amplifies albedo errors); that case only proves the SRV
	// returns linear values: the raw stored bytes read as linear would be off by > 1e-2 (several K).
	constexpr float HardwareDecodeTol = 2e-3f;
	struct FCase { const TCHAR* Name; int32 W, H, DW, DH; FIntPoint ColorPad, DepthPad; EBase Base; bool bSrgb; };
	const FCase Cases[] = {
		{ TEXT("same size"),                    64, 36, 64, 36, FIntPoint(0, 0), FIntPoint(0, 0), EBase::Float,        false },
		{ TEXT("half-res depth, offset rects"), 64, 36, 32, 18, FIntPoint(3, 2), FIntPoint(4, 3), EBase::Float,        false },
		{ TEXT("no base colour"),               64, 36, 64, 36, FIntPoint(0, 0), FIntPoint(0, 0), EBase::None,         false },
		{ TEXT("sRGB-encoded base colour"),     64, 36, 64, 36, FIntPoint(0, 0), FIntPoint(0, 0), EBase::Float,        true  },
		// GBufferC as it is at runtime (Task 1: LINEAR via the SRV's hardware decode): the shader must see linear values.
		{ TEXT("sRGB-format base texture"),     64, 36, 64, 36, FIntPoint(0, 0), FIntPoint(0, 0), EBase::HardwareSrgb, false },
	};
	for (const FCase& C : Cases)
	{
		CamSimThermalTest::FThermalTestScene S = CamSimThermalTest::MakeScene(C.W, C.H, C.DW, C.DH);
		S.P.bBaseColorSrgb = C.bSrgb ? 1u : 0u;
		TArray<uint8> BaseBytes;
		TArray<CamSimThermalRef::FPixelResult> RefRaw;   // HardwareSrgb: the reference fed the undecoded bytes
		if (C.Base == EBase::HardwareSrgb)
		{
			CamSimThermalTest::FThermalTestScene Raw = S;
			// The reference's input is what the hardware decode should return: the exact linear value of each byte.
			BaseBytes.SetNumUninitialized(S.Base.Num());
			for (int32 I = 0; I < S.Base.Num(); ++I)
			{
				BaseBytes[I] = LinearToSrgbByte(S.Base[I].R);
				const float Lin = CamSimThermalRef::SrgbToLinear(static_cast<float>(BaseBytes[I]) / 255.0f);
				S.Base[I] = FLinearColor(Lin, Lin, Lin, 1.0f);
				const float Enc = static_cast<float>(BaseBytes[I]) / 255.0f;
				Raw.Base[I] = FLinearColor(Enc, Enc, Enc, 1.0f);
			}
			RefRaw = CamSimThermalRef::Run(Raw.Images(true), Raw.P);
		}
		const float Tol = C.Base == EBase::HardwareSrgb ? HardwareDecodeTol : MirrorTol;
		const bool bBase = C.Base != EBase::None;
		const TArray<CamSimThermalRef::FPixelResult> Ref = CamSimThermalRef::Run(S.Images(bBase), S.P);
		FLayout L;
		L.ColorMin = C.ColorPad;
		L.ColorExtent = FIntPoint(C.W, C.H) + C.ColorPad * 2;
		L.DepthMin = C.DepthPad;
		L.DepthExtent = FIntPoint(C.DW, C.DH) + C.DepthPad * 2;
		const TArray<float> Gpu = RunThermalOnGpu(S, L, C.Base, BaseBytes);
		if (!TestEqual(*FString::Printf(TEXT("%s: readback"), C.Name), Gpu.Num(), C.W * C.H)) continue;
		int32 Bad = 0, WorstI = 0;
		float WorstRel = 0.0f, WorstRawRel = 0.0f;
		TSet<uint8> Classes;
		for (int32 I = 0; I < Gpu.Num(); ++I)
		{
			const float R = Ref[I].Radiance;
			const float Rel = FMath::Abs(Gpu[I] - R) / FMath::Max(FMath::Abs(R), 1e-6f);
			Classes.Add(static_cast<uint8>(Ref[I].Class));
			if (!(Rel <= Tol)) ++Bad;
			if (!(Rel <= WorstRel)) { WorstRel = Rel; WorstI = I; }
			if (RefRaw.Num() > 0) WorstRawRel = FMath::Max(WorstRawRel, FMath::Abs(Gpu[I] - RefRaw[I].Radiance) / FMath::Max(FMath::Abs(RefRaw[I].Radiance), 1e-6f));
		}
		if (RefRaw.Num() > 0)
		{
			TestTrue(*FString::Printf(TEXT("%s: GPU is far from the raw-byte reading (max relative %.3g > 1e-2)"), C.Name, WorstRawRel), WorstRawRel > 1e-2f);
		}
		AddInfo(FString::Printf(TEXT("%s: max relative error %.3g at (%d, %d), class %d"),
			C.Name, WorstRel, WorstI % C.W, WorstI / C.W, static_cast<int32>(Ref[WorstI].Class)));
		TestEqual(*FString::Printf(TEXT("%s: pixels beyond %.0e relative (worst %.3g at (%d, %d), class %d, gpu %.6g ref %.6g)"),
			C.Name, Tol, WorstRel, WorstI % C.W, WorstI / C.W, static_cast<int32>(Ref[WorstI].Class), Gpu[WorstI], Ref[WorstI].Radiance), Bad, 0);
		TestEqual(*FString::Printf(TEXT("%s: every class present"), C.Name), Classes.Num(), 5);
	}
	return true;
}
