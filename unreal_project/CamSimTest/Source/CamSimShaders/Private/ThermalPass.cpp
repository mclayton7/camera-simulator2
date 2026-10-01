// Copyright CamSim Contributors. All Rights Reserved.

#include "ThermalPass.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "SceneView.h"
#include "SystemTextures.h"

static_assert(FThermalFrameParams::LutSize == 1024, "LogLut[256] below and THERMAL_LUT_SIZE in CamSimThermalCommon.ush");
static_assert(FThermalFrameParams::MaxClasses == 32, "ClassData[32] below and in CamSimThermalCommon.ush");
static_assert(FThermalFrameParams::NumStencils == 256, "StencilData[128] below and in CamSimThermalCommon.ush");

/** ThermalCS parameters (CamSimThermal.usf + CamSimThermalCommon.ush). */
BEGIN_SHADER_PARAMETER_STRUCT(FCamSimThermalParameters, )
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER(FIntPoint, OutSize)
	SHADER_PARAMETER(FIntPoint, ColorViewMin)
	SHADER_PARAMETER(FIntPoint, DepthViewMin)
	SHADER_PARAMETER(FIntPoint, DepthViewSize)
	SHADER_PARAMETER(float, InputScale)
	SHADER_PARAMETER_ARRAY(FVector4f, LogLut, [256])
	SHADER_PARAMETER(float, LutMinK)
	SHADER_PARAMETER(float, LutMaxK)
	SHADER_PARAMETER(float, LutScale)
	SHADER_PARAMETER_ARRAY(FVector4f, ClassData, [32])
	SHADER_PARAMETER_ARRAY(FVector4f, StencilData, [128])
	SHADER_PARAMETER(uint32, NumClasses)
	SHADER_PARAMETER(uint32, TerrainClass)
	SHADER_PARAMETER(uint32, WaterClass)
	SHADER_PARAMETER(float, EntityDepthRatio)
	SHADER_PARAMETER(FMatrix44f, ClipToTranslatedWorld)
	SHADER_PARAMETER(FVector3f, Up)
	SHADER_PARAMETER(uint32, bWater)
	SHADER_PARAMETER(float, CamHeightCm)
	SHADER_PARAMETER(float, SeaRadiusCm)
	SHADER_PARAMETER(float, WaterBandCm)
	SHADER_PARAMETER(float, TairK)
	SHADER_PARAMETER(float, SkyEpsZ)
	SHADER_PARAMETER(float, Cloud)
	SHADER_PARAMETER(float, SkyHemiRadiance)
	SHADER_PARAMETER(float, BetaPerCm)
	SHADER_PARAMETER(float, KLum)
	SHADER_PARAMETER(float, EClampWm2)
	SHADER_PARAMETER(float, KFastScale)
	SHADER_PARAMETER(uint32, bBaseColorSrgb)
	SHADER_PARAMETER(uint32, UseBaseColor)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColor)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepth)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CustomDepth)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>, CustomStencil)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, BaseColor)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutRadiance)
	SHADER_PARAMETER(FIntPoint, OutputMin)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutSceneColor)
END_SHADER_PARAMETER_STRUCT()

/** Per-pixel temperature -> in-band radiance (ROADMAP 4A). */
class FCamSimThermalCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCamSimThermalCS);
	SHADER_USE_PARAMETER_STRUCT(FCamSimThermalCS, FGlobalShader);
	using FParameters = FCamSimThermalParameters;

	class FUseView : SHADER_PERMUTATION_BOOL("USE_VIEW");
	class FWriteSceneColor : SHADER_PERMUTATION_BOOL("WRITE_SCENE_COLOR");   // Task 17: radiance into scene colour (BeforeDOF)
	using FPermutationDomain = TShaderPermutationDomain<FUseView, FWriteSceneColor>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCamSimThermalCS, "/CamSim/Private/CamSimThermal.usf", "ThermalCS", SF_Compute);

bool IsThermalPassSupported(FString& OutWhy)
{
	if (GUsingNullRHI)
	{
		OutWhy = TEXT("NullRHI has no GPU");
		return false;
	}
	const FGlobalShaderMap* Map = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	if (!Map)
	{
		OutWhy = TEXT("no global shader map");
		return false;
	}
	for (int32 Perm = 0; Perm < FCamSimThermalCS::FPermutationDomain::PermutationCount; ++Perm)
	{
		if (!Map->HasShader(&FCamSimThermalCS::GetStaticType(), Perm))
		{
			OutWhy = FString::Printf(TEXT("FCamSimThermalCS permutation %d missing from the global shader map"), Perm);
			return false;
		}
	}
	return true;
}

FRDGTextureRef AddThermalPass(FRDGBuilder& GraphBuilder, const FThermalPassInputs& In, const FThermalFrameParams& P)
{
	check(In.SceneColor && In.SceneDepth && In.CustomDepth && In.CustomStencil);
	check(In.SceneColorRect.Area() > 0);
	check(In.ViewUniformBuffer || In.DepthViewRect.Area() > 0);   // an empty rect leaves the shader's clamp undefined
	const FIntPoint Out = In.SceneColorRect.Size();
	const bool bWriteSceneColor = In.OutputSceneColor != nullptr;
	if (bWriteSceneColor)
	{
		check(In.OutputSceneColor != In.SceneColor);
		check(In.OutputRect.Size() == Out);
		check(In.OutputRect.Min.X >= 0 && In.OutputRect.Min.Y >= 0
			&& In.OutputRect.Max.X <= In.OutputSceneColor->Desc.Extent.X && In.OutputRect.Max.Y <= In.OutputSceneColor->Desc.Extent.Y);
	}
	const FRDGTextureRef Radiance = bWriteSceneColor ? In.OutputSceneColor : GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(Out, PF_R32_FLOAT, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV),
		TEXT("CamSimThermalRadiance"));

	const bool bUseView = In.ViewUniformBuffer != nullptr;
	auto* Pass = GraphBuilder.AllocParameters<FCamSimThermalParameters>();
	if (bUseView)
	{
		Pass->View = TUniformBufferRef<FViewUniformShaderParameters>(In.ViewUniformBuffer);
	}
	Pass->OutSize       = Out;
	Pass->ColorViewMin  = In.SceneColorRect.Min;
	Pass->DepthViewMin  = In.DepthViewRect.Min;
	Pass->DepthViewSize = In.DepthViewRect.Size();
	Pass->InputScale    = P.InputScale;
	for (int32 I = 0; I < FThermalFrameParams::LutSize; ++I) Pass->LogLut[I / 4][I % 4] = P.LogLut[I];
	Pass->LutMinK  = FThermalFrameParams::LutMinK;
	Pass->LutMaxK  = FThermalFrameParams::LutMaxK;
	Pass->LutScale = FThermalFrameParams::LutScale;
	for (int32 C = 0; C < FThermalFrameParams::MaxClasses; ++C)
	{
		Pass->ClassData[C] = FVector4f(P.ClassTempK[C], P.ClassEmissivity[C], P.ClassKFast[C], P.ClassSAbsRef[C]);
	}
	for (int32 K = 0; K < FThermalFrameParams::NumStencils / 2; ++K)
	{
		Pass->StencilData[K] = FVector4f(static_cast<float>(P.StencilClass[2 * K]), P.StencilOffsetK[2 * K],
			static_cast<float>(P.StencilClass[2 * K + 1]), P.StencilOffsetK[2 * K + 1]);
	}
	Pass->NumClasses       = P.NumClasses;
	Pass->TerrainClass     = P.TerrainClass;
	Pass->WaterClass       = P.WaterClass;
	Pass->EntityDepthRatio = P.EntityDepthRatio;
	Pass->ClipToTranslatedWorld = P.ClipToTranslatedWorld;
	Pass->Up               = P.Up;
	Pass->bWater           = P.bWater;
	Pass->CamHeightCm      = P.CamHeightCm;
	Pass->SeaRadiusCm      = P.SeaRadiusCm;
	Pass->WaterBandCm      = P.WaterBandCm;
	Pass->TairK            = P.TairK;
	Pass->SkyEpsZ          = P.SkyEpsZ;
	Pass->Cloud            = P.Cloud;
	Pass->SkyHemiRadiance  = P.SkyHemiRadiance;
	Pass->BetaPerCm        = P.BetaPerCm;
	Pass->KLum             = P.KLum;
	Pass->EClampWm2        = P.EClampWm2;
	Pass->KFastScale       = P.KFastScale;
	Pass->bBaseColorSrgb   = P.bBaseColorSrgb;
	Pass->UseBaseColor     = In.BaseColor ? 1u : 0u;
	Pass->SceneColor       = In.SceneColor;
	Pass->SceneDepth       = In.SceneDepth;
	Pass->CustomDepth      = In.CustomDepth;
	Pass->CustomStencil    = In.CustomStencil;
	Pass->BaseColor        = In.BaseColor ? In.BaseColor : GSystemTextures.GetBlackDummy(GraphBuilder);
	if (bWriteSceneColor)
	{
		Pass->OutputMin     = In.OutputRect.Min;
		Pass->OutSceneColor = GraphBuilder.CreateUAV(Radiance);
	}
	else
	{
		Pass->OutRadiance   = GraphBuilder.CreateUAV(Radiance);
	}

	FCamSimThermalCS::FPermutationDomain Perm;
	Perm.Set<FCamSimThermalCS::FUseView>(bUseView);
	Perm.Set<FCamSimThermalCS::FWriteSceneColor>(bWriteSceneColor);
	TShaderMapRef<FCamSimThermalCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel), Perm);
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CamSimThermal %dx%d", Out.X, Out.Y), Shader, Pass,
		FComputeShaderUtils::GetGroupCount(Out, FIntPoint(8, 8)));
	return Radiance;
}
