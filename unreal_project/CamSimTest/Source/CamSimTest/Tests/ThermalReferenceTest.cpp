// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Tests/ThermalTestScene.h"

#include <limits>

// CamSim.Thermal.Reference.*: the per-pixel CPU reference ThermalCS mirrors (ROADMAP 4A).

using namespace CamSimThermalTest;
using CamSimThermalRef::EPixelClass;
using CamSimThermalRef::FPixelResult;
using CamSimThermalRef::FPixelSample;

namespace
{
	/** A terrain sample (plane 7 m below the camera) at (U, V) with the sunlit luminance and base colour 0.2. */
	FPixelSample TerrainSample(const FThermalFrameParams& P, const FRotator& Rot, float U, float V)
	{
		FPixelSample S;
		S.U = U; S.V = V;
		S.DeviceZ = DeviceZForPlane(P, Rot, U, V, -700.0f);
		S.bHasBase = true;
		S.Base = FVector3f(0.2f);
		S.Color = FVector3f(SunlitLuminance(P, 0.2f));
		return S;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefClassesTest, "CamSim.Thermal.Reference.ClassificationOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefClassesTest::RunTest(const FString& Parameters)
{
	const FThermalTestScene S = MakeScene(64, 36, 64, 36);
	const TArray<FPixelResult> R = CamSimThermalRef::Run(S.Images(true), S.P);
	if (!TestEqual(TEXT("one result per pixel"), R.Num(), S.W * S.H)) return false;
	auto At = [&](float U, float V) -> const FPixelResult& { return R[FMath::FloorToInt32(V * S.H) * S.W + FMath::FloorToInt32(U * S.W)]; };
	TestTrue(TEXT("top row: sky"), At(0.50f, 0.02f).Class == EPixelClass::Sky);
	TestTrue(TEXT("visible entity on terrain"), At(0.65f, 0.75f).Class == EPixelClass::Entity);
	TestTrue(TEXT("entity: class temperature + offset + its own fast term"), At(0.65f, 0.75f).TempK > S.P.ClassTempK[2] + 8.0f);
	TestTrue(TEXT("occluded entity: the terrain in front"), At(0.85f, 0.75f).Class == EPixelClass::Terrain);
	TestTrue(TEXT("visible entity over water beats water"), At(0.25f, 0.80f).Class == EPixelClass::Entity);
	TestTrue(TEXT("left half below the horizon: water"), At(0.10f, 0.70f).Class == EPixelClass::Water);
	TestTrue(TEXT("right half: terrain"), At(0.95f, 0.60f).Class == EPixelClass::Terrain);
	TestNearlyEqual(TEXT("water temperature = class"), At(0.10f, 0.70f).TempK, S.P.ClassTempK[1], 1e-4f);
	TestTrue(TEXT("sky colder than terrain"), At(0.50f, 0.02f).Radiance < At(0.95f, 0.60f).Radiance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefShadowTest, "CamSim.Thermal.Reference.FastTermShadowCooler",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefShadowTest::RunTest(const FString& Parameters)
{
	const FRotator Rot(-30.0, 0.0, 0.0);
	FThermalFrameParams P = MakeParams(Rot, 64, 36);
	P.bWater = 0;
	FPixelSample S = TerrainSample(P, Rot, 0.5f, 0.6f);
	const FPixelResult Sun = CamSimThermalRef::EvaluatePixel(P, S);
	TestNearlyEqual(TEXT("sunlit reference surface: fast term 0"), Sun.TempK, P.ClassTempK[0], 1e-3f);
	S.Color *= 0.25f;
	const FPixelResult Shade = CamSimThermalRef::EvaluatePixel(P, S);
	TestNearlyEqual(TEXT("shadow: k_fast (S_abs,pix - S_abs,ref)"), Shade.TempK - Sun.TempK, 0.015f * (0.25f * 400.0f - 400.0f), 1e-3f);
	TestTrue(TEXT("shadow radiance lower"), Shade.Radiance < Sun.Radiance);
	S.Color *= 6.0f;   // 1.5x the reference: a sun-facing slope
	TestTrue(TEXT("sun-facing slope warmer"), CamSimThermalRef::EvaluatePixel(P, S).TempK > Sun.TempK);
	S.bHasBase = false;
	TestNearlyEqual(TEXT("no base colour: no fast term"), CamSimThermalRef::EvaluatePixel(P, S).TempK, P.ClassTempK[0], 1e-6f);
	S.bHasBase = true;
	P.KFastScale = 0.0f;
	TestNearlyEqual(TEXT("KFastScale 0 (fallback): no fast term"), CamSimThermalRef::EvaluatePixel(P, S).TempK, P.ClassTempK[0], 1e-6f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefPathTest, "CamSim.Thermal.Reference.PathTermFarIsAir",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefPathTest::RunTest(const FString& Parameters)
{
	const FRotator Rot(-30.0, 0.0, 0.0);
	FThermalFrameParams P = MakeParams(Rot, 64, 36);
	P.bWater = 0;
	const FPixelSample S = TerrainSample(P, Rot, 0.5f, 0.6f);
	const float BAir = CamSimThermalRef::LutRadiance(P, P.TairK);
	P.BetaPerCm = 0.05f;   // tau = exp(-70) over ~14 m
	TestNearlyEqual(TEXT("opaque path: B(T_air)"), CamSimThermalRef::EvaluatePixel(P, S).Radiance, BAir, BAir * 1e-6f);
	P.BetaPerCm = 0.0f;
	const float Eps = P.ClassEmissivity[0];
	const float Expected = Eps * CamSimThermalRef::LutRadiance(P, P.ClassTempK[0]) + (1.0f - Eps) * P.SkyHemiRadiance;
	TestNearlyEqual(TEXT("no path: eps B(T) + (1 - eps) L_sky,hemi"), CamSimThermalRef::EvaluatePixel(P, S).Radiance, Expected, Expected * 1e-4f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefNaNTest, "CamSim.Thermal.Reference.NaNHandling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefNaNTest::RunTest(const FString& Parameters)
{
	const FRotator Rot(-30.0, 0.0, 0.0);
	const FThermalFrameParams P = MakeParams(Rot, 64, 36);
	const float BAir = CamSimThermalRef::LutRadiance(P, P.TairK);
	const float NaN = std::numeric_limits<float>::quiet_NaN(), Inf = std::numeric_limits<float>::infinity();
	struct FCase { const TCHAR* Name; float Z; FVector3f C; };
	for (const FCase& C : { FCase{ TEXT("NaN colour"), 0.01f, FVector3f(NaN, 1.0f, 1.0f) }, FCase{ TEXT("-Inf colour"), 0.01f, FVector3f(1.0f, -Inf, 1.0f) },
		FCase{ TEXT("NaN depth"), NaN, FVector3f(1.0f) }, FCase{ TEXT("+Inf depth"), Inf, FVector3f(1.0f) } })
	{
		FPixelSample S = TerrainSample(P, Rot, 0.5f, 0.6f);
		S.DeviceZ = C.Z;
		S.Color = C.C;
		const FPixelResult R = CamSimThermalRef::EvaluatePixel(P, S);
		TestTrue(*FString::Printf(TEXT("%s: invalid"), C.Name), R.Class == EPixelClass::Invalid);
		TestEqual(*FString::Printf(TEXT("%s: B(T_air)"), C.Name), R.Radiance, BAir);
	}
	const FThermalTestScene Scene = MakeScene(64, 36, 64, 36);
	int32 NonFinite = 0;
	for (const FPixelResult& R : CamSimThermalRef::Run(Scene.Images(true), Scene.P)) NonFinite += FMath::IsFinite(R.Radiance) ? 0 : 1;
	TestEqual(TEXT("no NaN/Inf reaches the detector"), NonFinite, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefHorizonTest, "CamSim.Thermal.Reference.SkyBelowHorizonAndCameraAtSeaLevel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefHorizonTest::RunTest(const FString& Parameters)
{
	// Review focus 2. From altitude, sky pixels can look below the geometric horizon (sin el < 0).
	const FRotator Level(0.0, 0.0, 0.0);
	FThermalFrameParams P = MakeParams(Level, 64, 36);
	FPixelSample Sky;
	Sky.U = 0.5f; Sky.V = 0.9f; Sky.DeviceZ = 0.0f;
	const FPixelResult R = CamSimThermalRef::EvaluatePixel(P, Sky);
	TestTrue(TEXT("below-horizon sky ray: sky"), R.Class == EPixelClass::Sky);
	TestTrue(TEXT("finite"), FMath::IsFinite(R.TempK) && FMath::IsFinite(R.Radiance));
	TestNearlyEqual(TEXT("clamped to MinSinEl"), R.TempK, CamSimThermalRef::SkyTemperatureK(P, 0.05f), 1e-4f);
	TestTrue(TEXT("between the zenith sky and T_air"), R.TempK >= CamSimThermalRef::SkyTemperatureK(P, 1.0f) && R.TempK <= P.TairK + 1e-3f);
	// A deck camera 30 cm below the still sea surface (wave trough): water 50 cm below the camera.
	const FRotator Down(-30.0, 0.0, 0.0);
	P = MakeParams(Down, 64, 36);
	P.CamHeightCm = -30.0f;
	FPixelSample W;
	W.U = 0.5f; W.V = 0.6f;
	W.DeviceZ = DeviceZForPlane(P, Down, W.U, W.V, -50.0f);
	const FPixelResult Rw = CamSimThermalRef::EvaluatePixel(P, W);
	TestTrue(TEXT("camera below the sea surface: water"), Rw.Class == EPixelClass::Water);
	TestTrue(TEXT("finite"), FMath::IsFinite(Rw.Radiance));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefExtremesTest, "CamSim.Thermal.Reference.FastTermExtremesFinite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefExtremesTest::RunTest(const FString& Parameters)
{
	// Review focus 5.
	const FRotator Rot(-30.0, 0.0, 0.0);
	FThermalFrameParams P = MakeParams(Rot, 64, 36);
	P.bWater = 0;
	FPixelSample S = TerrainSample(P, Rot, 0.5f, 0.6f);
	auto T = [&]() { return CamSimThermalRef::EvaluatePixel(P, S); };

	S.Base = FVector3f(0.0f);   // black paint: BaseLum clamps at 0.03, E at EClamp
	TestNearlyEqual(TEXT("black base colour: E clamped"), T().TempK, 300.0f + 0.015f * (1.0f * 1200.0f - 400.0f), 1e-3f);
	S.Base = FVector3f(0.2f);
	S.Color = FVector3f(1e30f);  // emissive / specular highlight
	TestNearlyEqual(TEXT("highlight: E clamped"), T().TempK, 300.0f + 0.015f * (0.8f * 1200.0f - 400.0f), 1e-3f);
	S.Color = FVector3f(-5.0f);  // negative scene colour
	TestNearlyEqual(TEXT("negative colour: E = 0"), T().TempK, 300.0f + 0.015f * (0.0f - 400.0f), 1e-3f);
	S.Color = FVector3f(SunlitLuminance(P, 0.2f));
	P.KLum = 0.0f;
	TestTrue(TEXT("K_lum 0: finite"), FMath::IsFinite(T().TempK) && FMath::IsFinite(T().Radiance));
	P.KLum = 120.0f;
	P.EClampWm2 = 0.0f;          // night: S = 0
	P.ClassSAbsRef[0] = 0.0f;
	TestEqual(TEXT("night: fast term exactly 0"), T().TempK, P.ClassTempK[0]);
	P.BetaPerCm = 0.0f;
	S.Stencil = 7; S.CustomZ = S.DeviceZ; P.StencilOffsetK[7] = 900.0f;   // past the LUT
	const float B1000 = FMath::Exp(P.LogLut[FThermalFrameParams::LutSize - 1]);
	TestNearlyEqual(TEXT("T > 1000 K saturates at B(1000 K)"), T().Radiance,
		P.ClassEmissivity[2] * B1000 + (1.0f - P.ClassEmissivity[2]) * P.SkyHemiRadiance, B1000 * 1e-5f);
	S.Stencil = 0;
	P.ClassTempK[0] = 100.0f;
	const float B150 = FMath::Exp(P.LogLut[0]);
	TestNearlyEqual(TEXT("T < 150 K clamps at B(150 K)"), T().Radiance,
		P.ClassEmissivity[0] * B150 + (1.0f - P.ClassEmissivity[0]) * P.SkyHemiRadiance, P.SkyHemiRadiance * 1e-5f);
	P.NumClasses = 1;
	S.Stencil = 9; S.CustomZ = S.DeviceZ;   // stencil table says class 2, but only one class exists
	TestTrue(TEXT("class index clamped below NumClasses"), FMath::IsFinite(T().Radiance) && T().TempK == P.ClassTempK[0] + P.StencilOffsetK[9]);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefMappingTest, "CamSim.Thermal.Reference.DepthResolutionMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefMappingTest::RunTest(const FString& Parameters)
{
	// Output 8 x 4, depth (render resolution) 4 x 2: output pixel (x, y) reads texel (floor(U * 4), floor(V * 2)).
	const FRotator Rot(-30.0, 0.0, 0.0);
	FThermalTestScene S;
	S.W = 8; S.H = 4; S.DW = 4; S.DH = 2;
	S.P = MakeParams(Rot, S.W, S.H);
	S.P.bWater = 0;
	for (int32 Ty = 0; Ty < S.DH; ++Ty)
		for (int32 Tx = 0; Tx < S.DW; ++Tx) S.Depth.Add(DeviceZForPlane(S.P, Rot, (Tx + 0.5f) / S.DW, (Ty + 0.5f) / S.DH, -700.0f));
	S.Custom.Init(0.0f, 8);
	S.Stencil.Init(0, 8);
	S.Base.Init(FLinearColor(0.2f, 0.2f, 0.2f, 1.0f), 8);
	S.Stencil[1 * 4 + 3] = 3;
	S.Custom[1 * 4 + 3] = S.Depth[1 * 4 + 3];
	S.Color.Init(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f), 32);
	const TArray<FPixelResult> R = CamSimThermalRef::Run(S.Images(true), S.P);
	for (const FIntPoint Px : { FIntPoint(6, 2), FIntPoint(7, 2), FIntPoint(6, 3), FIntPoint(7, 3) })
	{
		TestTrue(*FString::Printf(TEXT("pixel (%d, %d) reads texel (3, 1)"), Px.X, Px.Y), R[Px.Y * 8 + Px.X].Class == EPixelClass::Entity);
	}
	TestTrue(TEXT("pixel (5, 3) reads texel (2, 1)"), R[3 * 8 + 5].Class == EPixelClass::Terrain);
	TestTrue(TEXT("pixel (6, 1) reads texel (3, 0)"), R[1 * 8 + 6].Class == EPixelClass::Terrain);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefSrgbTest, "CamSim.Thermal.Reference.SrgbBaseColorDecode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefSrgbTest::RunTest(const FString& Parameters)
{
	TestNearlyEqual(TEXT("sRGB 0.5 -> linear"), CamSimThermalRef::SrgbToLinear(0.5f), 0.214041f, 1e-5f);
	TestNearlyEqual(TEXT("sRGB toe"), CamSimThermalRef::SrgbToLinear(0.02f), 0.02f / 12.92f, 1e-7f);
	const FRotator Rot(-30.0, 0.0, 0.0);
	FThermalFrameParams P = MakeParams(Rot, 64, 36);
	P.bWater = 0;
	FPixelSample S = TerrainSample(P, Rot, 0.5f, 0.6f);
	S.Base = FVector3f(0.5f);                                 // encoded value of linear 0.214
	S.Color = FVector3f(SunlitLuminance(P, 0.214041f));
	P.bBaseColorSrgb = 1;
	const float Decoded = CamSimThermalRef::EvaluatePixel(P, S).TempK;
	P.bBaseColorSrgb = 0;
	const float Raw = CamSimThermalRef::EvaluatePixel(P, S).TempK;
	// Decoded, the base colour (0.214) is the albedo the luminance was made with: the reference surface, fast term ~0.
	TestTrue(TEXT("decoded: the reference surface"), FMath::Abs(Decoded - P.ClassTempK[0]) < 0.01f);
	TestTrue(TEXT("raw (undecoded) value: far off"), FMath::Abs(Raw - P.ClassTempK[0]) > 2.0f);
	return true;
}
