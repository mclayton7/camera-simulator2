// Copyright CamSim Contributors. All Rights Reserved.

#include "InstanceIdPass.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "SceneView.h"

/** InstanceIdCS parameters (see CamSimInstanceId.usf). */
BEGIN_SHADER_PARAMETER_STRUCT(FCamSimInstanceIdParameters, )
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER(FIntPoint, OutputSize)
	SHADER_PARAMETER(FIntPoint, DepthViewMin)
	SHADER_PARAMETER(FIntPoint, DepthViewSize)
	SHADER_PARAMETER(FVector2f, HalfOut)
	SHADER_PARAMETER(float, FocalPx)
	SHADER_PARAMETER(float, K1)
	SHADER_PARAMETER(float, K2)
	SHADER_PARAMETER(uint32, WaterCut)
	SHADER_PARAMETER_ARRAY(FVector4f, WaterPlanes, [256])
	SHADER_PARAMETER(FMatrix44f, ClipToTranslatedWorld)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepth)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CustomDepth)
	// FSceneTextureUniformParameters::CustomStencilTexture's declared type, so Task 7 binds it unchanged.
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>, CustomStencil)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutIds)
END_SHADER_PARAMETER_STRUCT()

/** Output-space visible/amodal stencil ids, resampled through the sensor's lens distortion. */
class FCamSimInstanceIdCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCamSimInstanceIdCS);
	SHADER_USE_PARAMETER_STRUCT(FCamSimInstanceIdCS, FGlobalShader);
	using FParameters = FCamSimInstanceIdParameters;

	class FUseViewRect : SHADER_PERMUTATION_BOOL("USE_VIEW_RECT");
	using FPermutationDomain = TShaderPermutationDomain<FUseViewRect>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		// Same distortion inverse as SensorCS (UndistortScale in CamSimSensorCommon.ush).
		OutEnvironment.SetDefine(TEXT("NEWTON_ITERATIONS"), FSensorFrameParams::NewtonIterations);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCamSimInstanceIdCS, "/CamSim/Private/CamSimInstanceId.usf", "InstanceIdCS", SF_Compute);

bool IsInstanceIdPassSupported(FString& OutWhy)
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
	for (int32 Perm = 0; Perm < FCamSimInstanceIdCS::FPermutationDomain::PermutationCount; ++Perm)
	{
		if (!Map->HasShader(&FCamSimInstanceIdCS::GetStaticType(), Perm))
		{
			OutWhy = FString::Printf(TEXT("FCamSimInstanceIdCS permutation %d missing from the global shader map"), Perm);
			return false;
		}
	}
	return true;
}

FRDGBufferRef AddInstanceIdPass(FRDGBuilder& GraphBuilder, const FInstanceIdInputs& In, const FSensorFrameParams& P)
{
	const FIntPoint Out = In.OutputSize;
	check(In.SceneDepth && In.CustomDepth && In.CustomStencil);
	check(Out.X > 0 && Out.Y > 0 && Out.X % 2 == 0);
	check(In.ViewUniformBuffer || In.DepthViewRect.Area() > 0);   // an empty rect leaves the shader's clamp undefined
	const FRDGBufferRef Ids = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Out.X * Out.Y / 2),
		TEXT("CamSimInstanceIds"));

	const bool bUseView = In.ViewUniformBuffer != nullptr;
	auto* Pass = GraphBuilder.AllocParameters<FCamSimInstanceIdParameters>();
	if (bUseView)
	{
		Pass->View = TUniformBufferRef<FViewUniformShaderParameters>(In.ViewUniformBuffer);
	}
	Pass->OutputSize    = Out;
	Pass->DepthViewMin  = In.DepthViewRect.Min;
	Pass->DepthViewSize = In.DepthViewRect.Size();
	Pass->HalfOut       = FVector2f(0.5f * Out.X, 0.5f * Out.Y);   // as FCamSimSensorParameters::HalfOut
	Pass->FocalPx       = P.FocalPx;
	Pass->K1            = P.K1;
	Pass->K2            = P.K2;
	Pass->WaterCut      = In.bWaterCut ? 1u : 0u;
	for (int32 S = 0; S < 256; ++S) Pass->WaterPlanes[S] = In.WaterPlanes[S];
	Pass->ClipToTranslatedWorld = In.ClipToTranslatedWorld;
	Pass->SceneDepth    = In.SceneDepth;
	Pass->CustomDepth   = In.CustomDepth;
	Pass->CustomStencil = In.CustomStencil;
	Pass->OutIds        = GraphBuilder.CreateUAV(Ids);

	FCamSimInstanceIdCS::FPermutationDomain Perm;
	Perm.Set<FCamSimInstanceIdCS::FUseViewRect>(bUseView);
	TShaderMapRef<FCamSimInstanceIdCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel), Perm);
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CamSimInstanceIds %dx%d", Out.X, Out.Y), Shader, Pass,
		FComputeShaderUtils::GetGroupCount(FIntPoint(Out.X / 2, Out.Y), FIntPoint(8, 8)));
	return Ids;
}
