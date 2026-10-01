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
#include "ThermalPass.h"
#include "Tests/ThermalTestScene.h"

// CamSim.GPU.Thermal.*: ThermalCS (CamSimThermal.usf) against CamSimThermalRef on synthetic textures — sky, entity
// visible/occluded/over water, water, terrain, shadow, NaN/Inf — within 1e-4 relative radiance (ROADMAP 4A); the land-cover
// lookup, bilinear blend and base-colour refinement of terrain pixels (ROADMAP 4B, LandCoverMatchesCpu).

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

	/** The land-cover window as the runtime binds it: PF_R8_UINT, one code per texel. */
	FTextureRHIRef UploadCodes(FRHICommandListImmediate& RHICmdList, const TArray<uint8>& Codes, int32 N)
	{
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("CamSimTestLandCover"), N, N, PF_R8_UINT)
			.SetFlags(ETextureCreateFlags::ShaderResource).SetInitialState(ERHIAccess::SRVMask);
		FTextureRHIRef Tex = RHICmdList.CreateTexture(Desc);
		RHICmdList.UpdateTexture2D(Tex, 0, FUpdateTextureRegion2D(0, 0, 0, 0, N, N), N, Codes.GetData());
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
	 *  HardwareSrgb: base colour as the bytes of BaseBytes (DW x DH) in an sRGB-flagged 8-bit texture.
	 *  LandCover: the window codes (S.P.LandCoverTexels^2) bound as FThermalPassInputs::LandCover; null: none bound. */
	TArray<float> RunThermalOnGpu(const CamSimThermalTest::FThermalTestScene& S, const FLayout& L, EBase BaseMode,
		const TArray<uint8>& BaseBytes, const TArray<uint8>* LandCover = nullptr)
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
			const int32 LcN = static_cast<int32>(S.P.LandCoverTexels);
			FTextureRHIRef LcTex = LandCover ? UploadCodes(RHICmdList, *LandCover, LcN) : FTextureRHIRef();
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
				In.LandCover = LandCover ? GraphBuilder.RegisterExternalTexture(CreateRenderTarget(LcTex, TEXT("CamSimTestLandCover"))) : nullptr;
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

namespace
{
	/** Sentinel the scene-colour target is pre-filled with; texels outside OutputRect must keep it. */
	const FFloat16Color SceneColorSentinel(FLinearColor(-7.0f, 3.5f, -0.25f, 0.5f));

	/** ThermalCS's WRITE_SCENE_COLOR permutation: radiance into a pre-filled PF_FloatRGBA target at OutRect (same-size depth
	 *  and colour). Returns the whole target (Ext.X * Ext.Y texels); empty on a readback failure. */
	TArray<FFloat16Color> RunThermalToSceneColorOnGpu(const CamSimThermalTest::FThermalTestScene& S, FIntPoint TargetExt, FIntRect OutRect)
	{
		const FIntPoint Ext(S.W, S.H);
		TArray<FFloat16Color> Result;
		ENQUEUE_RENDER_COMMAND(CamSimThermalSceneColorGpuTest)([&](FRHICommandListImmediate& RHICmdList)
		{
			FTextureRHIRef ColorTex   = UploadRgba(RHICmdList, S.Color, Ext, TEXT("CamSimTestThermalColor"));
			FTextureRHIRef DepthTex   = UploadFloat(RHICmdList, S.Depth, Ext, TEXT("CamSimTestThermalDepth"));
			FTextureRHIRef CustomTex  = UploadFloat(RHICmdList, S.Custom, Ext, TEXT("CamSimTestThermalCustom"));
			FTextureRHIRef StencilTex = UploadStencil(RHICmdList, S.Stencil, Ext);
			FTextureRHIRef BaseTex    = UploadRgba(RHICmdList, S.Base, Ext, TEXT("CamSimTestThermalBase"));

			const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("CamSimTestThermalSceneColor"), TargetExt.X, TargetExt.Y, PF_FloatRGBA)
				.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV).SetInitialState(ERHIAccess::SRVMask);
			FTextureRHIRef Target = RHICmdList.CreateTexture(Desc);
			TArray<FFloat16Color> Fill;
			Fill.Init(SceneColorSentinel, TargetExt.X * TargetExt.Y);
			RHICmdList.UpdateTexture2D(Target, 0, FUpdateTextureRegion2D(0, 0, 0, 0, TargetExt.X, TargetExt.Y), TargetExt.X * sizeof(FFloat16Color),
				reinterpret_cast<const uint8*>(Fill.GetData()));

			FRHIGPUTextureReadback Rb(TEXT("CamSimTestThermalSceneColorReadback"));
			bool bSameTexture = false;
			{
				FRDGBuilder GraphBuilder(RHICmdList);
				FThermalPassInputs Ti;
				Ti.SceneColor     = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(ColorTex, TEXT("CamSimTestThermalColor")));
				Ti.SceneColorRect = FIntRect(FIntPoint::ZeroValue, Ext);
				Ti.SceneDepth     = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(DepthTex, TEXT("CamSimTestThermalDepth")));
				Ti.CustomDepth    = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(CustomTex, TEXT("CamSimTestThermalCustom")));
				const FRDGTextureRef StencilRdg = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(StencilTex, TEXT("CamSimTestThermalStencil")));
				Ti.CustomStencil  = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(StencilRdg));
				Ti.BaseColor      = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(BaseTex, TEXT("CamSimTestThermalBase")));
				Ti.DepthViewRect  = FIntRect(FIntPoint::ZeroValue, Ext);
				const FRDGTextureRef Out = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Target, TEXT("CamSimTestThermalSceneColor")));
				Ti.OutputSceneColor = Out;
				Ti.OutputRect       = OutRect;
				const FRDGTextureRef Written = AddThermalPass(GraphBuilder, Ti, S.P);
				bSameTexture = Written == Out;
				AddEnqueueCopyPass(GraphBuilder, &Rb, Written);
				GraphBuilder.Execute();
			}
			RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (!bSameTexture || !Rb.IsReady()) return;
			int32 Pitch = 0;
			const FFloat16Color* Data = static_cast<const FFloat16Color*>(Rb.Lock(Pitch));
			Result.SetNumUninitialized(TargetExt.X * TargetExt.Y);
			for (int32 Y = 0; Y < TargetExt.Y; ++Y)
				FMemory::Memcpy(&Result[Y * TargetExt.X], Data + static_cast<int64>(Y) * Pitch, TargetExt.X * sizeof(FFloat16Color));
			Rb.Unlock();
		});
		FlushRenderingCommands();
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalGpuSceneColorOutputTest, "CamSim.GPU.Thermal.SceneColorOutputMatchesCpu",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalGpuSceneColorOutputTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("skipped: NullRHI (run scripts/run_gpu_tests.sh)")); return true; }
	// Task 17 (BeforeDOF): ThermalCS writes float4(L, L, L, 1) into scene colour (fp16) at OutputRect, for TSR to resolve.
	// fp16 storage: M1 Pro/Metal converts the UAV store by truncation, not rounding (seen: 2.076061 stored as 2.074219),
	// so the bound is one fp16 ulp, 2^-10 = 9.8e-4 relative; the mirror itself is ~3e-6 here (MatchesCpu).
	constexpr float Fp16Tol = 1e-3f;
	constexpr int32 W = 64, H = 36;
	const CamSimThermalTest::FThermalTestScene S = CamSimThermalTest::MakeScene(W, H, W, H);
	const TArray<CamSimThermalRef::FPixelResult> Ref = CamSimThermalRef::Run(S.Images(true), S.P);
	const FIntPoint TargetExt(W + 11, H + 7);
	const FIntRect OutRect(FIntPoint(5, 3), FIntPoint(5 + W, 3 + H));
	const TArray<FFloat16Color> Gpu = RunThermalToSceneColorOnGpu(S, TargetExt, OutRect);
	if (!TestEqual(TEXT("readback of the returned (= OutputSceneColor) texture"), Gpu.Num(), TargetExt.X * TargetExt.Y)) return true;

	int32 Bad = 0, Untouched = 0, Clobbered = 0, WorstI = 0;
	float WorstRel = 0.0f, WorstGpu = 0.0f;
	for (int32 Y = 0; Y < TargetExt.Y; ++Y)
	{
		for (int32 X = 0; X < TargetExt.X; ++X)
		{
			const FFloat16Color& T = Gpu[Y * TargetExt.X + X];
			const bool bSentinel = T.R.Encoded == SceneColorSentinel.R.Encoded && T.G.Encoded == SceneColorSentinel.G.Encoded
				&& T.B.Encoded == SceneColorSentinel.B.Encoded && T.A.Encoded == SceneColorSentinel.A.Encoded;
			if (!OutRect.Contains(FIntPoint(X, Y)))
			{
				if (!bSentinel) ++Clobbered;
				continue;
			}
			if (bSentinel) { ++Untouched; continue; }
			const int32 I = (Y - OutRect.Min.Y) * W + (X - OutRect.Min.X);
			const float R = Ref[I].Radiance;
			const float Den = FMath::Max(FMath::Abs(R), 1e-6f);
			const float Rel = FMath::Max3(FMath::Abs(T.R.GetFloat() - R), FMath::Abs(T.G.GetFloat() - R), FMath::Abs(T.B.GetFloat() - R)) / Den;
			if (!(Rel <= Fp16Tol) || T.A.GetFloat() != 1.0f) ++Bad;
			if (!(Rel <= WorstRel)) { WorstRel = Rel; WorstI = I; WorstGpu = T.R.GetFloat(); }
		}
	}
	AddInfo(FString::Printf(TEXT("max relative error %.3g at (%d, %d), class %d (gpu %.7g, ref %.7g, ref as fp16 %.7g)"), WorstRel,
		WorstI % W, WorstI / W, static_cast<int32>(Ref[WorstI].Class), WorstGpu, Ref[WorstI].Radiance, FFloat16(Ref[WorstI].Radiance).GetFloat()));
	TestEqual(TEXT("texels outside OutputRect changed (must keep the sentinel)"), Clobbered, 0);
	TestEqual(TEXT("texels inside OutputRect never written"), Untouched, 0);
	TestEqual(*FString::Printf(TEXT("texels beyond %.0e relative (R = G = B = L) or A != 1 (worst %.3g)"), Fp16Tol, WorstRel), Bad, 0);
	return true;
}

namespace
{
	struct FEndToEndResult { TArray<float> Radiance; TArray<uint8> Nv12; bool bOk = false; };

	/** One graph, as the live capture: ThermalCS radiance -> SensorCS with bRadianceInput (same-size depth and colour). */
	FEndToEndResult RunThermalThroughSensorOnGpu(const CamSimThermalTest::FThermalTestScene& S, const FSensorFrameParams& SP)
	{
		FEndToEndResult Result;
		const FIntPoint Ext(S.W, S.H);
		ENQUEUE_RENDER_COMMAND(CamSimThermalEndToEndGpuTest)([&](FRHICommandListImmediate& RHICmdList)
		{
			FTextureRHIRef ColorTex   = UploadRgba(RHICmdList, S.Color, Ext, TEXT("CamSimTestThermalColor"));
			FTextureRHIRef DepthTex   = UploadFloat(RHICmdList, S.Depth, Ext, TEXT("CamSimTestThermalDepth"));
			FTextureRHIRef CustomTex  = UploadFloat(RHICmdList, S.Custom, Ext, TEXT("CamSimTestThermalCustom"));
			FTextureRHIRef StencilTex = UploadStencil(RHICmdList, S.Stencil, Ext);
			FTextureRHIRef BaseTex    = UploadRgba(RHICmdList, S.Base, Ext, TEXT("CamSimTestThermalBase"));
			FRHIGPUTextureReadback RadRb(TEXT("CamSimTestThermalRadiance"));
			FRHIGPUBufferReadback Nv12Rb(TEXT("CamSimTestThermalNv12"));
			uint32 Nv12Bytes = 0;
			{
				FRDGBuilder GraphBuilder(RHICmdList);
				FThermalPassInputs Ti;
				Ti.SceneColor     = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(ColorTex, TEXT("CamSimTestThermalColor")));
				Ti.SceneColorRect = FIntRect(FIntPoint::ZeroValue, Ext);
				Ti.SceneDepth     = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(DepthTex, TEXT("CamSimTestThermalDepth")));
				Ti.CustomDepth    = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(CustomTex, TEXT("CamSimTestThermalCustom")));
				const FRDGTextureRef StencilRdg = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(StencilTex, TEXT("CamSimTestThermalStencil")));
				Ti.CustomStencil  = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(StencilRdg));
				Ti.BaseColor      = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(BaseTex, TEXT("CamSimTestThermalBase")));
				Ti.DepthViewRect  = FIntRect(FIntPoint::ZeroValue, Ext);
				const FRDGTextureRef Radiance = AddThermalPass(GraphBuilder, Ti, S.P);

				FSensorGraphInputs In;
				In.SceneColor     = Radiance;
				In.SceneViewRect  = FIntRect(FIntPoint::ZeroValue, Ext);
				In.bRadianceInput = true;
				In.OutputSize     = Ext;
				const FSensorGraphOutputs Out = AddSensorPasses(GraphBuilder, In, SP);
				Nv12Bytes = Out.Nv12Bytes;
				AddEnqueueCopyPass(GraphBuilder, &RadRb, Radiance);
				AddEnqueueCopyPass(GraphBuilder, &Nv12Rb, Out.Nv12, Nv12Bytes);
				GraphBuilder.Execute();
			}
			RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (!RadRb.IsReady() || !Nv12Rb.IsReady()) return;
			int32 Pitch = 0;
			const float* Data = static_cast<const float*>(RadRb.Lock(Pitch));
			Result.Radiance.SetNumUninitialized(S.W * S.H);
			for (int32 Y = 0; Y < S.H; ++Y) FMemory::Memcpy(&Result.Radiance[Y * S.W], Data + static_cast<int64>(Y) * Pitch, S.W * sizeof(float));
			RadRb.Unlock();
			Result.Nv12.SetNumUninitialized(Nv12Bytes);
			FMemory::Memcpy(Result.Nv12.GetData(), Nv12Rb.Lock(Nv12Bytes), Nv12Bytes);
			Nv12Rb.Unlock();
			Result.bOk = true;
		});
		FlushRenderingCommands();
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalGpuEndToEndTest, "CamSim.GPU.Thermal.EndToEndRadianceToNv12",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalGpuEndToEndTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("skipped: NullRHI (run scripts/run_gpu_tests.sh)")); return true; }
	constexpr int32 W = 64, H = 36;
	CamSimThermalTest::FThermalTestScene S = CamSimThermalTest::MakeScene(W, H, W, H);
	S.P.KFastScale = 0.0f;   // night: no solar term (terrain 300 K, vehicle 295 + 8 K, sky ~ 0.77 emissivity at 288 K)
	// The scene's classes (CPU reference) define the regions; the NaN/Inf texels are classed Invalid and left out.
	const TArray<CamSimThermalRef::FPixelResult> Ref = CamSimThermalRef::Run(S.Images(true), S.P);

	FSensorFrameParams SP;   // default photon detector, no optics
	SP.Mode          = ESensorGraphMode::IR;
	SP.SignalWeights = FVector3f(1.0f, 0.0f, 0.0f);
	SP.InputScale    = 1.0f / CamSimThermalTest::MwirBand().Radiance(300.0f);   // as UCamSimCaptureComponent (GetSignalScale)
	SP.PhotonGain    = 0.5f;    // a 300 K scene at half full scale
	SP.DisplayGain   = 3.0f;    // AGC-like stretch: N 0.5 -> 0.5 display
	SP.DisplayOffset = -1.0f;
	const FEndToEndResult G = RunThermalThroughSensorOnGpu(S, SP);
	if (!TestTrue(TEXT("GPU readback"), G.bOk && G.Radiance.Num() == W * H && G.Nv12.Num() == W * H * 3 / 2)) return true;

	int32 NonFinite = 0;
	for (float L : G.Radiance) { if (!FMath::IsFinite(L)) ++NonFinite; }
	TestEqual(TEXT("non-finite radiance texels (NaN/Inf inputs are sanitized)"), NonFinite, 0);

	double Sum[5] = {}; int32 Count[5] = {};
	for (int32 I = 0; I < W * H; ++I)
	{
		const int32 C = static_cast<int32>(Ref[I].Class);
		Sum[C] += G.Nv12[I];
		++Count[C];
	}
	auto Mean = [&](CamSimThermalRef::EPixelClass C) { const int32 K = static_cast<int32>(C); return Count[K] > 0 ? Sum[K] / Count[K] : -1.0; };
	const double SkyY = Mean(CamSimThermalRef::EPixelClass::Sky);
	const double TerrainY = Mean(CamSimThermalRef::EPixelClass::Terrain);
	const double EntityY = Mean(CamSimThermalRef::EPixelClass::Entity);
	AddInfo(FString::Printf(TEXT("mean Y: sky %.1f (%d px), terrain %.1f (%d px), entity %.1f (%d px)"),
		SkyY, Count[0], TerrainY, Count[3], EntityY, Count[1]));
	if (!TestTrue(TEXT("every region present"), Count[0] > 0 && Count[1] > 0 && Count[3] > 0)) return true;
	TestTrue(*FString::Printf(TEXT("white-hot: sky (%.1f) darker than terrain (%.1f)"), SkyY, TerrainY), SkyY < TerrainY);
	TestTrue(*FString::Printf(TEXT("night: entity (%.1f) brighter than terrain (%.1f)"), EntityY, TerrainY), EntityY > TerrainY);
	TestTrue(*FString::Printf(TEXT("terrain mid-grey (16..235 limited range): %.1f"), TerrainY), TerrainY > 40.0 && TerrainY < 220.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalGpuLandCoverTest, "CamSim.GPU.Thermal.LandCoverMatchesCpu",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalGpuLandCoverTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("skipped: NullRHI (run scripts/run_gpu_tests.sh)")); return true; }
	constexpr float MirrorTol = 1e-4f;   // ThermalCS vs CamSimThermalRef (never loosen it)
	struct FCase { const TCHAR* Name; float Yaw; int32 DW, DH; FIntPoint ColorPad, DepthPad; bool bBase; bool bLandCover; };
	const FCase Cases[] = {
		{ TEXT("yaw 0, refined"),               0.0f,    64, 36, FIntPoint(0, 0), FIntPoint(0, 0), true,  true  },
		{ TEXT("yaw 30, refined"),              30.0f,   64, 36, FIntPoint(0, 0), FIntPoint(0, 0), true,  true  },
		{ TEXT("yaw 30, no base colour"),       30.0f,   64, 36, FIntPoint(0, 0), FIntPoint(0, 0), false, true  },
		{ TEXT("half-res depth, offset rects"), -117.0f, 32, 18, FIntPoint(3, 2), FIntPoint(4, 3), true,  true  },
		{ TEXT("land cover off, window bound"), 30.0f,   64, 36, FIntPoint(0, 0), FIntPoint(0, 0), true,  false },
	};
	for (const FCase& C : Cases)
	{
		CamSimThermalTest::FThermalTestScene S = CamSimThermalTest::MakeLandCoverScene(64, 36, C.DW, C.DH, C.Yaw);
		if (!C.bLandCover) S.P.bLandCover = 0;
		const TArray<CamSimThermalRef::FPixelResult> Ref = CamSimThermalRef::Run(S.Images(C.bBase), S.P);
		FLayout L;
		L.ColorMin = C.ColorPad;
		L.ColorExtent = FIntPoint(S.W, S.H) + C.ColorPad * 2;
		L.DepthMin = C.DepthPad;
		L.DepthExtent = FIntPoint(C.DW, C.DH) + C.DepthPad * 2;
		const EBase Base = C.bBase ? EBase::Float : EBase::None;
		const TArray<float> Gpu = RunThermalOnGpu(S, L, Base, TArray<uint8>(), &S.LandCover);
		if (!TestEqual(*FString::Printf(TEXT("%s: readback"), C.Name), Gpu.Num(), S.W * S.H)) continue;
		int32 Bad = 0, Land = 0, WorstI = 0;
		float WorstRel = 0.0f;
		for (int32 I = 0; I < Gpu.Num(); ++I)
		{
			const float R = Ref[I].Radiance;
			const float Rel = FMath::Abs(Gpu[I] - R) / FMath::Max(FMath::Abs(R), 1e-6f);
			Land += Ref[I].bLandCover ? 1 : 0;
			if (!(Rel <= MirrorTol)) ++Bad;
			if (!(Rel <= WorstRel)) { WorstRel = Rel; WorstI = I; }
		}
		AddInfo(FString::Printf(TEXT("%s: worst relative %.2e at pixel (%d, %d), %d land-cover pixels"), C.Name, WorstRel, WorstI % S.W, WorstI / S.W, Land));
		TestEqual(*FString::Printf(TEXT("%s: every pixel within %.0e of CamSimThermalRef"), C.Name, MirrorTol), Bad, 0);
		if (C.bLandCover)
		{
			TestTrue(*FString::Printf(TEXT("%s: land cover exercised"), C.Name), Land >= 300);
		}
		else
		{
			const TArray<float> Unbound = RunThermalOnGpu(S, L, Base, TArray<uint8>(), nullptr);
			TestTrue(TEXT("bLandCover = 0: bit for bit the same as no window bound"),
				Unbound.Num() == Gpu.Num() && FMemory::Memcmp(Unbound.GetData(), Gpu.GetData(), Gpu.Num() * sizeof(float)) == 0);
		}
	}
	return true;
}
