// Copyright CamSim Contributors. All Rights Reserved.

#include "SensorGraph.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "SceneView.h"
#include "SystemTextures.h"

class FCamSimSensorApplyCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCamSimSensorApplyCS);
	SHADER_USE_PARAMETER_STRUCT(FCamSimSensorApplyCS, FGlobalShader);

	class FUseViewPreExposure : SHADER_PERMUTATION_BOOL("USE_VIEW_PREEXPOSURE");
	using FPermutationDomain = TShaderPermutationDomain<FUseViewPreExposure>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColor)
		SHADER_PARAMETER_SAMPLER(SamplerState, SceneColorSampler)
		SHADER_PARAMETER(FVector2f, SceneUvMin)
		SHADER_PARAMETER(FVector2f, SceneUvSize)
		SHADER_PARAMETER(FIntPoint, SceneViewMin)
		SHADER_PARAMETER(uint32, bExactInput)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, Bloom)
		SHADER_PARAMETER_SAMPLER(SamplerState, BloomSampler)
		SHADER_PARAMETER(FVector2f, BloomUvMin)
		SHADER_PARAMETER(FVector2f, BloomUvSize)
		SHADER_PARAMETER(float, BloomWeight)
		SHADER_PARAMETER(float, InputScale)
		SHADER_PARAMETER(FIntPoint, OutputSize)
		SHADER_PARAMETER(uint32, Mode)
		SHADER_PARAMETER(uint32, bBlackHot)
		SHADER_PARAMETER(float, PhotonGain)
		SHADER_PARAMETER(float, AnalogGain)
		SHADER_PARAMETER(float, DisplayGain)
		SHADER_PARAMETER(float, DisplayOffset)
		SHADER_PARAMETER(FVector3f, SignalWeights)
		SHADER_PARAMETER(float, KneeStart)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutSensorRgb)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutHistogram)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCamSimSensorApplyCS, "/CamSim/Private/CamSimSensor.usf", "ApplyCS", SF_Compute);

class FCamSimSensorPackNv12CS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCamSimSensorPackNv12CS);
	SHADER_USE_PARAMETER_STRUCT(FCamSimSensorPackNv12CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SensorRgb)
		SHADER_PARAMETER(FIntPoint, OutputSize)
		SHADER_PARAMETER(uint32, Mode)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutNv12)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCamSimSensorPackNv12CS, "/CamSim/Private/CamSimSensor.usf", "PackNv12CS", SF_Compute);

bool IsSensorGraphSupported(FString& OutWhy)
{
	if (GUsingNullRHI)
	{
		OutWhy = TEXT("NullRHI has no GPU");
		return false;
	}
	if (GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5)
	{
		OutWhy = TEXT("the RHI's max feature level is below SM5 (no compute)");
		return false;
	}
	const FGlobalShaderMap* Map = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	if (!Map)
	{
		OutWhy = TEXT("no global shader map");
		return false;
	}
	for (int32 Perm = 0; Perm < FCamSimSensorApplyCS::FPermutationDomain::PermutationCount; ++Perm)
	{
		if (!Map->HasShader(&FCamSimSensorApplyCS::GetStaticType(), Perm))
		{
			OutWhy = FString::Printf(TEXT("FCamSimSensorApplyCS permutation %d missing from the global shader map"), Perm);
			return false;
		}
	}
	if (!Map->HasShader(&FCamSimSensorPackNv12CS::GetStaticType(), 0))
	{
		OutWhy = TEXT("FCamSimSensorPackNv12CS missing from the global shader map");
		return false;
	}
	return true;
}

FSensorGraphOutputs AddSensorPasses(FRDGBuilder& GraphBuilder, const FSensorGraphInputs& In, const FSensorFrameParams& P)
{
	check(In.SceneColor && In.OutputSize.X % 4 == 0 && In.OutputSize.Y % 2 == 0);
	RDG_EVENT_SCOPE(GraphBuilder, "CamSimSensor");
	const FIntPoint Out = In.OutputSize;

	FSensorGraphOutputs Result;
	Result.SensorRgb = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(Out, PF_FloatRGBA, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV),
		TEXT("CamSimSensorRgb"));
	Result.Histogram = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FSensorHistogram::NumBins),
		TEXT("CamSimSensorHistogram"));
	Result.Nv12Bytes = static_cast<uint32>(Out.X * Out.Y * 3 / 2);
	Result.Nv12 = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Result.Nv12Bytes / 4),
		TEXT("CamSimSensorNv12"));
	FRDGBufferUAVRef HistUav = GraphBuilder.CreateUAV(Result.Histogram);
	AddClearUAVPass(GraphBuilder, HistUav, 0u);

	const FIntPoint SrcExtent = In.SceneColor->Desc.Extent;
	const FIntPoint SrcSize = In.SceneViewRect.Size();
	{
		const bool bUseView = In.ViewUniformBuffer != nullptr;
		auto* Pass = GraphBuilder.AllocParameters<FCamSimSensorApplyCS::FParameters>();
		if (bUseView)
		{
			Pass->View = TUniformBufferRef<FViewUniformShaderParameters>(In.ViewUniformBuffer);
		}
		Pass->SceneColor        = In.SceneColor;
		Pass->SceneColorSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();
		Pass->SceneUvMin        = FVector2f(In.SceneViewRect.Min) / FVector2f(SrcExtent);
		Pass->SceneUvSize       = FVector2f(SrcSize) / FVector2f(SrcExtent);
		Pass->SceneViewMin      = In.SceneViewRect.Min;
		Pass->bExactInput       = (SrcSize == Out) ? 1u : 0u;
		const bool bBloom = In.Bloom != nullptr;
		Pass->Bloom        = bBloom ? In.Bloom : GSystemTextures.GetBlackDummy(GraphBuilder);
		Pass->BloomSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();
		if (bBloom)
		{
			const FVector2f BloomExtent(In.Bloom->Desc.Extent);
			Pass->BloomUvMin  = FVector2f(In.BloomViewRect.Min) / BloomExtent;
			Pass->BloomUvSize = FVector2f(In.BloomViewRect.Size()) / BloomExtent;
		}
		Pass->BloomWeight   = bBloom ? 1.0f : 0.0f;
		Pass->InputScale    = P.InputScale;
		Pass->OutputSize    = Out;
		Pass->Mode          = static_cast<uint32>(P.Mode);
		Pass->bBlackHot     = P.bBlackHot;
		Pass->PhotonGain    = P.PhotonGain;
		Pass->AnalogGain    = P.AnalogGain;
		Pass->DisplayGain   = P.DisplayGain;
		Pass->DisplayOffset = P.DisplayOffset;
		Pass->SignalWeights = P.SignalWeights;
		Pass->KneeStart     = P.KneeStart;
		Pass->OutSensorRgb  = GraphBuilder.CreateUAV(Result.SensorRgb);
		Pass->OutHistogram  = HistUav;

		FCamSimSensorApplyCS::FPermutationDomain Perm;
		Perm.Set<FCamSimSensorApplyCS::FUseViewPreExposure>(bUseView);
		TShaderMapRef<FCamSimSensorApplyCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel), Perm);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Apply %dx%d", Out.X, Out.Y), Shader, Pass,
			FComputeShaderUtils::GetGroupCount(Out, FIntPoint(8, 8)));
	}
	{
		auto* Pass = GraphBuilder.AllocParameters<FCamSimSensorPackNv12CS::FParameters>();
		Pass->SensorRgb  = Result.SensorRgb;
		Pass->OutputSize = Out;
		Pass->Mode       = static_cast<uint32>(P.Mode);
		Pass->OutNv12    = GraphBuilder.CreateUAV(Result.Nv12);
		TShaderMapRef<FCamSimSensorPackNv12CS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("PackNv12"), Shader, Pass,
			FComputeShaderUtils::GetGroupCount(FIntPoint(Out.X / 4, Out.Y / 2), FIntPoint(8, 8)));
	}
	return Result;
}
