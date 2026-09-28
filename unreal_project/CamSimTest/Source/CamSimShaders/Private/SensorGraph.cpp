// Copyright CamSim Contributors. All Rights Reserved.

#include "SensorGraph.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "SceneView.h"
#include "SystemTextures.h"
#include "SensorHash.h"

/** SensorCS parameters (see CamSimSensor.usf). */
BEGIN_SHADER_PARAMETER_STRUCT(FCamSimSensorParameters, )
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER(FIntPoint, OutputSize)
	SHADER_PARAMETER(FVector3f, SignalWeights)
	// Optics
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColor)
	SHADER_PARAMETER(FIntPoint, SceneViewMin)
	SHADER_PARAMETER(FIntPoint, SceneSize)
	SHADER_PARAMETER(FVector2f, SrcScale)
	SHADER_PARAMETER(FVector2f, HalfOut)
	SHADER_PARAMETER(uint32, bSameSize)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, Bloom)
	SHADER_PARAMETER(FIntPoint, BloomViewMin)
	SHADER_PARAMETER(FIntPoint, BloomSize)
	SHADER_PARAMETER(FVector2f, BloomScale)
	SHADER_PARAMETER(uint32, bBloom)
	SHADER_PARAMETER(float, InputScale)
	SHADER_PARAMETER(float, FocalPx)
	SHADER_PARAMETER(float, K1)
	SHADER_PARAMETER(float, K2)
	SHADER_PARAMETER(float, VignettingExponent)
	SHADER_PARAMETER_ARRAY(FVector4f, PsfTaps, [3])
	SHADER_PARAMETER(uint32, NumPsfTaps)
	// Detector + display
	SHADER_PARAMETER(uint32, Mode)
	SHADER_PARAMETER(uint32, bBlackHot)
	SHADER_PARAMETER(float, PhotonGain)
	SHADER_PARAMETER(float, AnalogGain)
	SHADER_PARAMETER(float, DisplayGain)
	SHADER_PARAMETER(float, DisplayOffset)
	SHADER_PARAMETER(float, KneeStart)
	SHADER_PARAMETER(uint32, DetectorType)
	SHADER_PARAMETER(float, FullWellE)
	SHADER_PARAMETER(float, ReadNoiseE)
	SHADER_PARAMETER(float, Prnu)
	SHADER_PARAMETER(float, DsnuE)
	SHADER_PARAMETER(float, DarkE)
	SHADER_PARAMETER(float, TemporalNoise)
	SHADER_PARAMETER(float, PixelFpn)
	SHADER_PARAMETER(float, ColumnFpn)
	SHADER_PARAMETER(float, RowFpn)
	SHADER_PARAMETER(float, AdcMax)
	SHADER_PARAMETER(float, InvAdcMax)
	SHADER_PARAMETER(float, HotFraction)
	SHADER_PARAMETER(float, DeadFraction)
	SHADER_PARAMETER_ARRAY(FUintVector4, DetectorKeys, [9])
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutSensorRgb)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutHistogram)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutNv12)
END_SHADER_PARAMETER_STRUCT()

/** Steps 1-8, one pass: sanitize, scale, [+bloom], distortion resample, cos^n, histogram,
 *  [PSF blur], detector, ADC, defects, display, NV12. */
class FCamSimSensorCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCamSimSensorCS);
	SHADER_USE_PARAMETER_STRUCT(FCamSimSensorCS, FGlobalShader);
	using FParameters = FCamSimSensorParameters;

	class FUseViewPreExposure : SHADER_PERMUTATION_BOOL("USE_VIEW_PREEXPOSURE");
	/** Largest blur radius the groupshared tile holds: 0 = no blur; 3 covers the presets (sigma_o <= 2/3 px)
	 *  with a small tile (better occupancy); 8 = CamSimOptics::MaxPsfRadius. A 5 class was measured
	 *  (3B.2 Task 10: R4/R5 at 1080p 2.38/2.58 -> 2.15/2.37 ms p95) and left out: still over the 2 ms
	 *  budget, so R > 3 stays a config warning (FCamSimConfig::ValidateWarnings). */
	class FBlurMaxRadius : SHADER_PERMUTATION_SPARSE_INT("BLUR_MAX_R", 0, 3, 8);
	using FPermutationDomain = TShaderPermutationDomain<FUseViewPreExposure, FBlurMaxRadius>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		// The distortion inverse runs exactly CamSimOptics::NewtonIterations steps, like the CPU reference.
		OutEnvironment.SetDefine(TEXT("NEWTON_ITERATIONS"), FSensorFrameParams::NewtonIterations);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCamSimSensorCS, "/CamSim/Private/CamSimSensor.usf", "SensorCS", SF_Compute);

/** CamSimHash::StreamKey for every stream SensorCS draws; layout documented at DetectorKeys in CamSimSensor.usf. */
template <typename TKeys>
static void FillDetectorKeys(const FSensorFrameParams& P, TKeys& Keys)
{
	using namespace CamSimHash;
	uint32 Flat[36] = {};
	auto SetPair = [&](uint32 Pair, uint32 Frame, uint32 Stream)
	{
		Flat[Pair * 2]     = StreamKey(Frame, P.Seed, Stream * 2u);
		Flat[Pair * 2 + 1] = StreamKey(Frame, P.Seed, Stream * 2u + 1u);
	};
	for (uint32 C = 0; C < 3; ++C)
	{
		const uint32 Base = C * 16u;
		SetPair(C * 4u + 0u, FixedFrame,   Base + 1u);   // PRNU
		SetPair(C * 4u + 1u, P.FrameIndex, Base + 2u);   // shot
		SetPair(C * 4u + 2u, FixedFrame,   Base + 3u);   // DSNU
		SetPair(C * 4u + 3u, P.FrameIndex, Base + 4u);   // read
	}
	SetPair(12u, P.FrameIndex, 5u);   // bolometer temporal
	SetPair(13u, FixedFrame,   6u);   // pixel FPN
	SetPair(14u, FixedFrame,   7u);   // column FPN
	SetPair(15u, FixedFrame,   8u);   // row FPN
	Flat[32] = StreamKey(FixedFrame, P.Seed, 2u * 9u);   // defects: raw draw of stream 9
	for (int32 I = 0; I < 9; ++I) Keys[I] = FUintVector4(Flat[4 * I], Flat[4 * I + 1], Flat[4 * I + 2], Flat[4 * I + 3]);
}

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
	for (int32 Perm = 0; Perm < FCamSimSensorCS::FPermutationDomain::PermutationCount; ++Perm)
	{
		if (!Map->HasShader(&FCamSimSensorCS::GetStaticType(), Perm))
		{
			OutWhy = FString::Printf(TEXT("FCamSimSensorCS permutation %d missing from the global shader map"), Perm);
			return false;
		}
	}
	return true;
}

FSensorGraphOutputs AddSensorPasses(FRDGBuilder& GraphBuilder, const FSensorGraphInputs& In, const FSensorFrameParams& P)
{
	check(In.SceneColor && In.OutputSize.X % 4 == 0 && In.OutputSize.Y % 2 == 0);
	check(P.NumPsfTaps >= 1 && P.NumPsfTaps <= static_cast<uint32>(FSensorFrameParams::MaxPsfTaps));
	RDG_EVENT_SCOPE(GraphBuilder, "CamSimSensor");
	const FIntPoint Out = In.OutputSize;
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

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

	const FIntPoint SrcSize = In.SceneViewRect.Size();
	{
		const bool bUseView = In.ViewUniformBuffer != nullptr;
		auto* Pass = GraphBuilder.AllocParameters<FCamSimSensorParameters>();
		if (bUseView)
		{
			Pass->View = TUniformBufferRef<FViewUniformShaderParameters>(In.ViewUniformBuffer);
		}
		Pass->OutputSize    = Out;
		Pass->SignalWeights = P.SignalWeights;

		Pass->SceneColor   = In.SceneColor;
		Pass->SceneViewMin = In.SceneViewRect.Min;
		Pass->SceneSize    = SrcSize;
		// Same float expressions as CamSimSensorRef::Optics.
		Pass->SrcScale     = FVector2f(static_cast<float>(SrcSize.X) / Out.X, static_cast<float>(SrcSize.Y) / Out.Y);
		Pass->HalfOut      = FVector2f(0.5f * Out.X, 0.5f * Out.Y);
		Pass->bSameSize    = (SrcSize == Out) ? 1u : 0u;
		const bool bBloom = In.Bloom != nullptr && In.BloomViewRect.Width() > 0 && In.BloomViewRect.Height() > 0;
		Pass->Bloom        = bBloom ? In.Bloom : GSystemTextures.GetBlackDummy(GraphBuilder);
		Pass->bBloom       = bBloom ? 1u : 0u;
		if (bBloom)
		{
			const FIntPoint BloomSize = In.BloomViewRect.Size();
			Pass->BloomViewMin = In.BloomViewRect.Min;
			Pass->BloomSize    = BloomSize;
			Pass->BloomScale   = FVector2f(static_cast<float>(BloomSize.X) / SrcSize.X, static_cast<float>(BloomSize.Y) / SrcSize.Y);
		}
		Pass->InputScale         = P.InputScale;
		Pass->FocalPx            = P.FocalPx;
		Pass->K1                 = P.K1;
		Pass->K2                 = P.K2;
		Pass->VignettingExponent = P.VignettingExponent;
		for (int32 K = 0; K < 12; ++K)
		{
			Pass->PsfTaps[K / 4][K % 4] = K < static_cast<int32>(P.NumPsfTaps) ? P.PsfTaps[K] : 0.0f;
		}
		Pass->NumPsfTaps    = P.NumPsfTaps;

		Pass->Mode          = static_cast<uint32>(P.Mode);
		Pass->bBlackHot     = P.bBlackHot;
		Pass->PhotonGain    = P.PhotonGain;
		Pass->AnalogGain    = P.AnalogGain;
		Pass->DisplayGain   = P.DisplayGain;
		Pass->DisplayOffset = P.DisplayOffset;
		Pass->KneeStart     = P.KneeStart;
		Pass->DetectorType  = P.DetectorType;
		Pass->FullWellE     = P.FullWellE;
		Pass->ReadNoiseE    = P.ReadNoiseE;
		Pass->Prnu          = P.Prnu;
		Pass->DsnuE         = P.DsnuE;
		Pass->DarkE         = P.DarkE;
		Pass->TemporalNoise = P.TemporalNoise;
		Pass->PixelFpn      = P.PixelFpn;
		Pass->ColumnFpn     = P.ColumnFpn;
		Pass->RowFpn        = P.RowFpn;
		Pass->AdcMax        = P.AdcMax;
		Pass->InvAdcMax     = 1.0f / P.AdcMax;   // as CamSimSensorRef::Run
		Pass->HotFraction   = P.HotFraction;
		Pass->DeadFraction  = P.DeadFraction;
		FillDetectorKeys(P, Pass->DetectorKeys);
		Pass->OutHistogram  = HistUav;

		Pass->OutSensorRgb  = GraphBuilder.CreateUAV(Result.SensorRgb);
		Pass->OutNv12       = GraphBuilder.CreateUAV(Result.Nv12);

		const int32 Radius = static_cast<int32>(P.NumPsfTaps) - 1;
		FCamSimSensorCS::FPermutationDomain Perm;
		Perm.Set<FCamSimSensorCS::FUseViewPreExposure>(bUseView);
		Perm.Set<FCamSimSensorCS::FBlurMaxRadius>(Radius == 0 ? 0 : Radius <= 3 ? 3 : 8);
		TShaderMapRef<FCamSimSensorCS> Shader(ShaderMap, Perm);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Sensor %dx%d R%d", Out.X, Out.Y, Radius), Shader, Pass,
			FComputeShaderUtils::GetGroupCount(Out, FIntPoint(16, 16)));
	}
	return Result;
}
