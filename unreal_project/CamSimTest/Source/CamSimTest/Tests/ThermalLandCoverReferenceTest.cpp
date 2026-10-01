// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Tests/ThermalTestScene.h"

#include <limits>

// CamSim.Thermal.Reference.LandCover* / Refinement*: land-cover lookup, smoothstep-bilinear material blend, geo-anchored
// domain warp (Task 13), base-colour refinement and the unchanged 4A path in the per-pixel CPU reference (ROADMAP 4B).

using namespace CamSimThermalTest;
using CamSimThermalRef::EPixelClass;
using CamSimThermalRef::FLandCoverSample;
using CamSimThermalRef::FPixelResult;
using CamSimThermalRef::FPixelSample;

namespace
{
	const FRotator Down(-50.0, 0.0, 0.0);

	TArray<uint8> Uniform(uint32 Texels, uint8 Code) { TArray<uint8> C; C.Init(Code, Texels * Texels); return C; }

	FVector4f Data(const FThermalFrameParams& P, uint32 C) { return CamSimThermalRef::ClassData(P, C); }

	bool Near4(const FVector4f& A, const FVector4f& B, float Tol)
	{
		return FMath::Abs(A.X - B.X) <= Tol && FMath::Abs(A.Y - B.Y) <= Tol && FMath::Abs(A.Z - B.Z) <= Tol && FMath::Abs(A.W - B.W) <= Tol;
	}

	/** Refined class data of a single-code window at its centre. */
	FVector4f Refined(const FThermalFrameParams& P, uint8 Code, const FVector3f& Base, bool bRefine = true)
	{
		const TArray<uint8> Codes = Uniform(P.LandCoverTexels, Code);
		const FLandCoverSample L = CamSimThermalRef::SampleLandCover(P, Codes.GetData(), GroundPoint(P, 0.0f, 0.0f, -700.0f));
		return CamSimThermalRef::BlendLandCover(P, L, Base, bRefine);
	}

	/** What Run samples for output pixel (X, Y). */
	FPixelSample SampleAt(const FThermalTestScene& S, int32 X, int32 Y, bool bBase, const uint8* LandCover)
	{
		FPixelSample Smp;
		Smp.U = (X + 0.5f) / S.W;
		Smp.V = (Y + 0.5f) / S.H;
		const int32 Tx = FMath::Clamp(FMath::FloorToInt32(Smp.U * S.DW), 0, S.DW - 1);
		const int32 Ty = FMath::Clamp(FMath::FloorToInt32(Smp.V * S.DH), 0, S.DH - 1);
		const int32 Ti = Ty * S.DW + Tx;
		Smp.DeviceZ = S.Depth[Ti];
		Smp.CustomZ = S.Custom[Ti];
		Smp.Stencil = S.Stencil[Ti];
		const FLinearColor& C = S.Color[Y * S.W + X];
		Smp.Color = FVector3f(C.R, C.G, C.B) * S.P.InputScale;
		if (bBase) { Smp.Base = FVector3f(S.Base[Ti].R, S.Base[Ti].G, S.Base[Ti].B); Smp.bHasBase = true; }
		Smp.LandCover = LandCover;
		return Smp;
	}

	/** ThermalReference.cpp's EvaluatePixel radiance as of 4A (git 3e8dcc0), frozen: land cover off must reproduce it bit for bit. */
	float Legacy4ARadiance(const FThermalFrameParams& P, const FPixelSample& S)
	{
		using namespace CamSimThermalRef;
		const float BAir = LutRadiance(P, P.TairK);
		if (!FMath::IsFinite(S.DeviceZ) || !FMath::IsFinite(S.Color.X) || !FMath::IsFinite(S.Color.Y) || !FMath::IsFinite(S.Color.Z)) return BAir;
		const float Nx = S.U * 2.0f - 1.0f, Ny = 1.0f - S.V * 2.0f;
		if (S.DeviceZ <= 0.0f)
		{
			const FVector3f Pn = ClipToWorld(P, Nx, Ny, 1.0f);
			const float Len = FMath::Sqrt(FVector3f::DotProduct(Pn, Pn));
			const float SinEl = (Len > 0.0f) ? FVector3f::DotProduct(Pn, P.Up) / Len : 1.0f;
			return LutRadiance(P, SkyTemperatureK(P, SinEl));
		}
		const FVector3f Pw = ClipToWorld(P, Nx, Ny, S.DeviceZ);
		const float Range = FMath::Sqrt(FVector3f::DotProduct(Pw, Pw));
		uint32 Class = P.TerrainClass;
		float Offset = 0.0f;
		const uint32 Stencil = S.Stencil & 0xFFu;
		if (Stencil > 0u && S.CustomZ >= S.DeviceZ * P.EntityDepthRatio)
		{
			Class = P.StencilClass[Stencil];
			Offset = P.StencilOffsetK[Stencil];
		}
		else if (P.bWater != 0u)
		{
			const float Vert = FVector3f::DotProduct(Pw, P.Up);
			const float D2 = FMath::Max(Range * Range - Vert * Vert, 0.0f);
			const float Height = P.CamHeightCm + Vert + D2 / (2.0f * P.SeaRadiusCm);
			if (Height < P.WaterBandCm) Class = P.WaterClass;
		}
		Class = FMath::Min(Class, FMath::Max(P.NumClasses, 1u) - 1u);
		float T = P.ClassTempK[Class] + Offset;
		if (S.bHasBase && P.KFastScale > 0.0f)
		{
			const FVector3f Base = (P.bBaseColorSrgb != 0u)
				? FVector3f(SrgbToLinear(S.Base.X), SrgbToLinear(S.Base.Y), SrgbToLinear(S.Base.Z)) : S.Base;
			const float BaseLum = Lum709(Base);
			const float E = FMath::Clamp(3.14159265f * Lum709(S.Color) / (FMath::Max(BaseLum, 0.03f) * FMath::Max(P.KLum, 1e-6f)), 0.0f, P.EClampWm2);
			const float SAbs = (1.0f - FMath::Clamp(BaseLum, 0.0f, 1.0f)) * E;
			T += P.KFastScale * P.ClassKFast[Class] * (SAbs - P.ClassSAbsRef[Class]);
		}
		const float Eps = P.ClassEmissivity[Class];
		const float LSurf = Eps * LutRadiance(P, T) + (1.0f - Eps) * P.SkyHemiRadiance;
		const float Tau = FMath::Exp(-P.BetaPerCm * Range);
		return Tau * LSurf + (1.0f - Tau) * BAir;
	}

	bool SameBits(float A, float B) { return FMemory::Memcmp(&A, &B, sizeof(float)) == 0; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefLandCoverBlendTest, "CamSim.Thermal.Reference.LandCoverBilinearBlend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefLandCoverBlendTest::RunTest(const FString& Parameters)
{
	FThermalFrameParams P = MakeLandCoverParams(Down, 64, 36, 8, 10.0f, 0.0f);
	P.bLandCoverRefine = 0;
	P.LandCoverCamOffsetM = FVector2f::ZeroVector;   // exact texel coordinates below
	TArray<uint8> Codes = Uniform(8, 30);   // columns 0..3 vegetation, 4..7 built-up
	for (int32 Y = 0; Y < 8; ++Y) for (int32 X = 4; X < 8; ++X) Codes[Y * 8 + X] = 50;
	auto At = [&](float E, float N) { return CamSimThermalRef::SampleLandCover(P, Codes.GetData(), GroundPoint(P, E, N, -700.0f)); };
	const FLandCoverSample Mid = At(0.0f, 0.0f);   // halfway between texel centres x = 3 (E = -5 m) and x = 4 (+5 m)
	if (!TestTrue(TEXT("inside"), Mid.bInside)) return false;
	TestNearlyEqual(TEXT("weights sum to 1"), Mid.Weights[0] + Mid.Weights[1] + Mid.Weights[2] + Mid.Weights[3], 1.0f, 1e-6f);
	const FVector4f Half = CamSimThermalRef::BlendLandCover(P, Mid, FVector3f(0.2f), false);
	TestNearlyEqual(TEXT("temperature: half vegetation, half built-up"), Half.X, 0.5f * 296.0f + 0.5f * 307.0f, 1e-3f);
	TestNearlyEqual(TEXT("emissivity blended, not the code"), Half.Y, 0.5f * 0.98f + 0.5f * 0.93f, 1e-5f);
	TestNearlyEqual(TEXT("k_fast blended"), Half.Z, 0.5f * 0.008f + 0.5f * 0.018f, 1e-6f);
	TestNearlyEqual(TEXT("S_abs,ref blended"), Half.W, 380.0f, 1e-3f);
	// Smoothstep blend fractions (Task 13): a quarter texel from the vegetation centre, S(0.25) = 0.15625 toward built-up.
	const FVector4f Quarter = CamSimThermalRef::BlendLandCover(P, At(-2.5f, 0.0f), FVector3f(0.2f), false);
	TestNearlyEqual(TEXT("a quarter texel toward vegetation (smoothstep)"), Quarter.X, 0.84375f * 296.0f + 0.15625f * 307.0f, 1e-3f);
	const FLandCoverSample Centre = At(-5.0f, 5.0f);   // exactly texel (3, 3)'s centre
	TestNearlyEqual(TEXT("texel centre: weight 1 on it"), Centre.Weights[0], 1.0f, 1e-6f);
	TestEqual(TEXT("texel centre: its class"), CamSimThermalRef::BlendLandCover(P, Centre, FVector3f(0.2f), false).X, 296.0f);

	// Through EvaluatePixel: the terrain class data is exactly the blend at the pixel's world position.
	P.KFastScale = 0.0f;
	FPixelSample S;
	S.U = 0.5f; S.V = 0.6f;
	S.DeviceZ = DeviceZForPlane(P, Down, S.U, S.V, -700.0f);
	S.LandCover = Codes.GetData();
	const FVector3f Pw = CamSimThermalRef::ClipToWorld(P, S.U * 2.0f - 1.0f, 1.0f - S.V * 2.0f, S.DeviceZ);
	const FVector4f Expect = CamSimThermalRef::BlendLandCover(P, CamSimThermalRef::SampleLandCover(P, Codes.GetData(), Pw), FVector3f(0.0f), false);
	const FPixelResult R = CamSimThermalRef::EvaluatePixel(P, S);
	TestTrue(TEXT("terrain pixel used land cover"), R.bLandCover && R.Class == EPixelClass::Terrain);
	TestEqual(TEXT("its temperature is the blend's"), R.TempK, Expect.X);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefLandCoverOrientationTest, "CamSim.Thermal.Reference.LandCoverOrientation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefLandCoverOrientationTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Codes = MakeLandCoverCodes(8);   // NW 10, NE 50, SW 60, SE 30
	for (const float Yaw : { 0.0f, 30.0f, -117.0f })
	{
		const FThermalFrameParams P = MakeLandCoverParams(Down, 64, 36, 8, 10.0f, Yaw);
		struct FCase { float E, N; uint8 Code; const TCHAR* Name; };
		for (const FCase& C : { FCase{ 20.0f, 20.0f, 50, TEXT("north-east") }, FCase{ -20.0f, 20.0f, 10, TEXT("north-west") },
			FCase{ -20.0f, -20.0f, 60, TEXT("south-west") }, FCase{ 20.0f, -20.0f, 30, TEXT("south-east") } })
		{
			const FLandCoverSample L = CamSimThermalRef::SampleLandCover(P, Codes.GetData(), GroundPoint(P, C.E, C.N, -700.0f));
			const bool bAll = L.bInside && L.Codes[0] == C.Code && L.Codes[1] == C.Code && L.Codes[2] == C.Code && L.Codes[3] == C.Code;
			TestTrue(*FString::Printf(TEXT("yaw %.0f: %s quadrant"), Yaw, C.Name), bAll);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefRefineVegTest, "CamSim.Thermal.Reference.RefinementVegetationBothDirections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefRefineVegTest::RunTest(const FString& Parameters)
{
	const FThermalFrameParams P = MakeLandCoverParams(Down, 64, 36, 8, 10.0f, 0.0f);
	const FVector3f Green(0.05f, 0.20f, 0.04f), Grey(0.2f), Half(0.2f, 0.24f, 0.2f);   // ExG ~1.07, 0, 0.125
	TestEqual(TEXT("green: v = 1"), CamSimThermalRef::RefinementWeights(P, Green).X, 1.0f);
	TestEqual(TEXT("grey: v = 0"), CamSimThermalRef::RefinementWeights(P, Grey).X, 0.0f);
	TestNearlyEqual(TEXT("ExG 0.125: v = 0.5"), CamSimThermalRef::RefinementWeights(P, Half).X, 0.5f, 1e-3f);
	TestTrue(TEXT("built-up + green (street trees, lawns) -> vegetation"), Near4(Refined(P, 50, Green), Data(P, LcVegetation), 1e-4f));
	TestTrue(TEXT("tree cover + grey (trail, clearing) -> bare soil"), Near4(Refined(P, 10, Grey), Data(P, LcBare), 1e-4f));
	TestTrue(TEXT("tree cover + green -> tree"), Near4(Refined(P, 10, Green), Data(P, LcTree), 1e-4f));
	TestTrue(TEXT("bare + green -> vegetation"), Near4(Refined(P, 60, Green), Data(P, LcVegetation), 1e-4f));
	TestTrue(TEXT("bare + grey -> bare"), Near4(Refined(P, 60, Grey), Data(P, LcBare), 1e-4f));
	const FVector4f Mix = Refined(P, 10, Half);
	TestNearlyEqual(TEXT("tree cover at v = 0.5: half tree, half bare"), Mix.X, 0.5f * 293.0f + 0.5f * 312.0f, 0.05f);
	TestTrue(TEXT("water code is never refined"), Near4(Refined(P, 80, Grey), Data(P, 1), 1e-4f));
	TestTrue(TEXT("no-data code is never refined"), Near4(Refined(P, 0, Green), Data(P, 0), 1e-4f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefRefineSplitTest, "CamSim.Thermal.Reference.RefinementAsphaltConcreteSplit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefRefineSplitTest::RunTest(const FString& Parameters)
{
	const FThermalFrameParams P = MakeLandCoverParams(Down, 64, 36, 8, 10.0f, 0.0f);
	TestTrue(TEXT("dark built-up -> asphalt"), Near4(Refined(P, 50, FVector3f(0.05f)), Data(P, LcAsphalt), 1e-4f));
	TestTrue(TEXT("bright built-up -> concrete"), Near4(Refined(P, 50, FVector3f(0.40f)), Data(P, LcConcrete), 1e-4f));
	TestNearlyEqual(TEXT("at asphalt_max_luma: half and half"), Refined(P, 50, FVector3f(P.AsphaltMaxLuma)).X, 0.5f * 310.0f + 0.5f * 304.0f, 0.01f);
	TestNearlyEqual(TEXT("concrete weight ramps over 0.04"), CamSimThermalRef::RefinementWeights(P, FVector3f(P.AsphaltMaxLuma + 0.01f)).Y, 0.75f, 1e-3f);
	TestTrue(TEXT("bare ground is not split into asphalt (decision: built-up only)"), Near4(Refined(P, 60, FVector3f(0.05f)), Data(P, LcBare), 1e-4f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefLandCoverNoBaseTest, "CamSim.Thermal.Reference.LandCoverWithoutBaseColour",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefLandCoverNoBaseTest::RunTest(const FString& Parameters)
{
	FThermalFrameParams P = MakeLandCoverParams(Down, 64, 36, 8, 10.0f, 0.0f);
	P.KFastScale = 0.0f;
	const TArray<uint8> BuiltUp = Uniform(8, 50);
	FPixelSample S;
	S.U = 0.5f; S.V = 0.6f;
	S.DeviceZ = DeviceZForPlane(P, Down, S.U, S.V, -700.0f);
	S.LandCover = BuiltUp.GetData();
	S.Base = FVector3f(0.05f, 0.20f, 0.04f);   // ignored: not bound
	S.bHasBase = false;
	TestNearlyEqual(TEXT("no base colour: WorldCover class alone (built_up)"), CamSimThermalRef::EvaluatePixel(P, S).TempK, P.ClassTempK[LcBuiltUp], 1e-3f);
	S.bHasBase = true;
	P.bLandCoverRefine = 0;
	TestNearlyEqual(TEXT("refinement off: built_up"), CamSimThermalRef::EvaluatePixel(P, S).TempK, P.ClassTempK[LcBuiltUp], 1e-3f);
	P.bLandCoverRefine = 1;
	TestNearlyEqual(TEXT("refinement on (green): vegetation, even with the fast term off (night)"),
		CamSimThermalRef::EvaluatePixel(P, S).TempK, P.ClassTempK[LcVegetation], 1e-3f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefLandCoverOffTest, "CamSim.Thermal.Reference.LandCoverOffIs4A",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefLandCoverOffTest::RunTest(const FString& Parameters)
{
	FThermalTestScene S = MakeScene(64, 36, 32, 18);   // sky, entities, water, terrain, shadow, NaN/Inf
	const TArray<uint8> Codes = MakeLandCoverCodes(32);
	FThermalFrameParams On = MakeLandCoverParams(S.ViewRot, 64, 36, 32, 10.0f, 30.0f);   // classes 0-2 = MakeParams'
	On.bWater = S.P.bWater;
	On.LandCoverWarpAmpM = 6.0f;   // the warp (Task 13) must not leak into the off path either
	On.LandCoverWarpCellM = 20.0f;
	On.LandCoverAnchorM = FVector2f(1234.5f, -876.25f);
	FThermalFrameParams Off = On;
	Off.bLandCover = 0;
	int32 Bad4A = 0, BadOff = 0, BadNonTerrain = 0, NonTerrain = 0, BadOutside = 0, Outside = 0;
	for (const bool bBase : { false, true })
	{
		for (int32 Y = 0; Y < S.H; ++Y)
		{
			for (int32 X = 0; X < S.W; ++X)
			{
				const FPixelSample None = SampleAt(S, X, Y, bBase, nullptr);
				const FPixelSample With = SampleAt(S, X, Y, bBase, Codes.GetData());
				Bad4A  += SameBits(CamSimThermalRef::EvaluatePixel(S.P, None).Radiance, Legacy4ARadiance(S.P, None)) ? 0 : 1;
				BadOff += SameBits(CamSimThermalRef::EvaluatePixel(Off, With).Radiance, Legacy4ARadiance(Off, With)) ? 0 : 1;
				const FPixelResult R = CamSimThermalRef::EvaluatePixel(On, With);
				if (R.Class != EPixelClass::Terrain) { ++NonTerrain; BadNonTerrain += SameBits(R.Radiance, Legacy4ARadiance(On, With)) ? 0 : 1; }
				else if (!R.bLandCover) { ++Outside; BadOutside += SameBits(R.Radiance, Legacy4ARadiance(On, With)) ? 0 : 1; }
			}
		}
	}
	TestEqual(TEXT("4A params, no window: bit for bit the 4A reference"), Bad4A, 0);
	TestEqual(TEXT("bLandCover = 0 with a window bound: bit for bit 4A"), BadOff, 0);
	TestTrue(TEXT("scene has sky / entity / water pixels"), NonTerrain > 100);
	TestEqual(TEXT("land cover on: sky, entity, water pixels unchanged"), BadNonTerrain, 0);
	TestTrue(TEXT("scene has terrain outside the window"), Outside > 0);
	TestEqual(TEXT("land cover on: terrain outside the window unchanged"), BadOutside, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefLandCoverEdgesTest, "CamSim.Thermal.Reference.LandCoverEdgesAndDegenerateAxes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefLandCoverEdgesTest::RunTest(const FString& Parameters)
{
	FThermalFrameParams P = MakeLandCoverParams(Down, 64, 36, 8, 10.0f, 0.0f);   // window E, N in [-40, 40] m; centres at +-35
	P.LandCoverCamOffsetM = FVector2f::ZeroVector;   // exact texel coordinates at the edges
	const TArray<uint8> Codes = MakeLandCoverCodes(8);
	auto At = [&](float E, float N) { return CamSimThermalRef::SampleLandCover(P, Codes.GetData(), GroundPoint(P, E, N, -700.0f)); };
	TestFalse(TEXT("west of the first texel centre: outside"), At(-35.5f, 0.0f).bInside);
	TestTrue (TEXT("first texel centre: inside"), At(-35.0f, 0.0f).bInside);
	const FLandCoverSample Last = At(35.0f, 0.0f);
	TestTrue (TEXT("last texel centre: inside"), Last.bInside);
	TestNearlyEqual(TEXT("... all weight on the last column"), Last.Weights[1] + Last.Weights[3], 1.0f, 1e-6f);
	TestFalse(TEXT("east of the last texel centre: outside"), At(35.5f, 0.0f).bInside);
	TestFalse(TEXT("north of the first row centre: outside"), At(0.0f, 35.5f).bInside);
	TestFalse(TEXT("null codes: off"), CamSimThermalRef::SampleLandCover(P, nullptr, GroundPoint(P, 0.0f, 0.0f, -700.0f)).bInside);
	FThermalFrameParams Q = P;
	Q.bLandCover = 0;
	TestFalse(TEXT("bLandCover 0: off"), CamSimThermalRef::SampleLandCover(Q, Codes.GetData(), GroundPoint(P, 0.0f, 0.0f, -700.0f)).bInside);
	Q = P;
	Q.LandCoverTexels = 1;
	TestFalse(TEXT("1-texel window: off"), CamSimThermalRef::SampleLandCover(Q, Codes.GetData(), FVector3f(0.0f, 0.0f, -700.0f)).bInside);
	Q = P;
	Q.LandCoverCamOffsetM.X = std::numeric_limits<float>::quiet_NaN();
	TestFalse(TEXT("NaN offset: outside, never NaN"), CamSimThermalRef::SampleLandCover(Q, Codes.GetData(), FVector3f(0.0f, 0.0f, -700.0f)).bInside);
	Q = P;
	Q.LandCoverEast = FVector3f(std::numeric_limits<float>::infinity(), 0.0f, 0.0f);
	TestFalse(TEXT("Inf axis: outside"), CamSimThermalRef::SampleLandCover(Q, Codes.GetData(), FVector3f(100.0f, 0.0f, -700.0f)).bInside);

	// A terrain pixel outside the window: TerrainClass, exactly as 4A.
	P.KFastScale = 0.0f;
	P.LandCoverCamOffsetM = FVector2f(1000.0f, 1000.0f);   // window 1 km away
	FPixelSample S;
	S.U = 0.5f; S.V = 0.6f;
	S.DeviceZ = DeviceZForPlane(P, Down, S.U, S.V, -700.0f);
	S.LandCover = Codes.GetData();
	const FPixelResult R = CamSimThermalRef::EvaluatePixel(P, S);
	TestFalse(TEXT("outside: no land cover"), R.bLandCover);
	TestEqual(TEXT("outside: terrain_default"), R.TempK, P.ClassTempK[P.TerrainClass]);
	return true;
}

// Review Focus 5: degenerate base colours and thresholds keep the blend convex and the radiance finite.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefRefineExtremesTest, "CamSim.Thermal.Reference.RefinementExtremesFinite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefRefineExtremesTest::RunTest(const FString& Parameters)
{
	FThermalFrameParams P = MakeLandCoverParams(Down, 64, 36, 8, 10.0f, 0.0f);
	const TArray<uint8> Codes = MakeLandCoverCodes(8);
	const FLandCoverSample Four = CamSimThermalRef::SampleLandCover(P, Codes.GetData(), GroundPoint(P, 0.0f, 0.0f, -700.0f));
	TestTrue(TEXT("window centre touches four quadrant codes"), Four.bInside && Four.Codes[0] != Four.Codes[3]);
	FVector4f Lo(1e9f), Hi(-1e9f);
	for (uint32 C = 0; C < P.NumClasses; ++C)
	{
		const FVector4f D = Data(P, C);
		Lo = FVector4f(FMath::Min(Lo.X, D.X), FMath::Min(Lo.Y, D.Y), FMath::Min(Lo.Z, D.Z), FMath::Min(Lo.W, D.W));
		Hi = FVector4f(FMath::Max(Hi.X, D.X), FMath::Max(Hi.Y, D.Y), FMath::Max(Hi.Z, D.Z), FMath::Max(Hi.W, D.W));
	}
	const float BMin = CamSimThermalRef::LutRadiance(P, FThermalFrameParams::LutMinK);
	const float BMax = CamSimThermalRef::LutRadiance(P, FThermalFrameParams::LutMaxK);
	int32 Bad = 0;
	for (const bool bDegenerate : { false, true })
	{
		P.VegIndexHi = bDegenerate ? P.VegIndexLo : 0.20f;
		P.AsphaltRampLuma = bDegenerate ? 0.0f : 0.04f;
		for (const uint32 bSrgb : { 0u, 1u })
		{
			P.bBaseColorSrgb = bSrgb;
			for (const FVector3f Base : { FVector3f(0.0f), FVector3f(1.0f), FVector3f(1.0f, 0.0f, 0.0f), FVector3f(0.0f, 1.0f, 0.0f), FVector3f(1e-7f) })
			{
				const FVector2f Wt = CamSimThermalRef::RefinementWeights(P, Base);
				Bad += (FMath::IsFinite(Wt.X) && FMath::IsFinite(Wt.Y) && Wt.X >= 0.0f && Wt.X <= 1.0f && Wt.Y >= 0.0f && Wt.Y <= 1.0f) ? 0 : 1;
				const FVector4f Cd = CamSimThermalRef::BlendLandCover(P, Four, Base, true);
				Bad += (Cd.X >= Lo.X - 1e-3f && Cd.X <= Hi.X + 1e-3f && Cd.Y >= Lo.Y - 1e-6f && Cd.Y <= Hi.Y + 1e-6f
					&& Cd.Z >= Lo.Z - 1e-6f && Cd.Z <= Hi.Z + 1e-6f && Cd.W >= Lo.W - 1e-3f && Cd.W <= Hi.W + 1e-3f) ? 0 : 1;
				FPixelSample S;
				S.U = 0.5f; S.V = 0.6f;
				S.DeviceZ = DeviceZForPlane(P, Down, S.U, S.V, -700.0f);
				S.LandCover = Codes.GetData();
				S.Base = Base;
				S.bHasBase = true;
				S.Color = FVector3f(SunlitLuminance(P, 0.2f));
				const float L = CamSimThermalRef::EvaluatePixel(P, S).Radiance;
				Bad += (FMath::IsFinite(L) && L >= BMin * 0.999f && L <= BMax * 1.001f) ? 0 : 1;
			}
		}
	}
	TestEqual(TEXT("weights in [0, 1], blend convex, radiance finite and in the LUT range"), Bad, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefLandCoverRunTest, "CamSim.Thermal.Reference.LandCoverRunCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefLandCoverRunTest::RunTest(const FString& Parameters)
{
	for (const float Yaw : { 0.0f, 30.0f })
	{
		const FThermalTestScene S = MakeLandCoverScene(64, 36, 64, 36, Yaw);
		const TArray<FPixelResult> R = CamSimThermalRef::Run(S.Images(true), S.P);
		int32 Inside = 0, Outside = 0, Entity = 0, EntityWithLc = 0;
		TSet<int32> Temps;
		for (const FPixelResult& X : R)
		{
			if (X.Class == EPixelClass::Entity) { ++Entity; EntityWithLc += X.bLandCover ? 1 : 0; }
			else if (X.Class == EPixelClass::Terrain) { X.bLandCover ? ++Inside : ++Outside; if (X.bLandCover) Temps.Add(FMath::FloorToInt32(X.TempK * 10.0f)); }
		}
		TestTrue(*FString::Printf(TEXT("yaw %.0f: terrain inside the window"), Yaw), Inside > 500);
		TestTrue(*FString::Printf(TEXT("yaw %.0f: terrain outside the window"), Yaw), Outside > 0);
		TestTrue(*FString::Printf(TEXT("yaw %.0f: an entity"), Yaw), Entity > 0);
		TestEqual(*FString::Printf(TEXT("yaw %.0f: entities never use land cover"), Yaw), EntityWithLc, 0);
		TestTrue(*FString::Printf(TEXT("yaw %.0f: varied land temperatures (%d)"), Yaw, Temps.Num()), Temps.Num() >= 8);
	}
	return true;
}

// ---- Task 13: break the 10 m grid (smoothstep fractions + geo-anchored domain warp) ----

namespace
{
	/** Blended temperature along East at North = N (no refinement), window coordinates in metres. */
	float TempAt(const FThermalFrameParams& P, const TArray<uint8>& Codes, float E, float N)
	{
		const FLandCoverSample L = CamSimThermalRef::SampleLandCover(P, Codes.GetData(), GroundPoint(P, E, N, -700.0f));
		return L.bInside ? CamSimThermalRef::BlendLandCover(P, L, FVector3f(0.2f), false).X : -1.0f;
	}

	/** max |second difference| at the texel centre E = Ec (samples within one step) / max elsewhere (more than 2 steps away). */
	float CreaseRatio(TFunctionRef<float(float)> T, float Ec, float Lo, float Hi, float Step)
	{
		float AtCentre = 0.0f, Elsewhere = 0.0f;
		for (float E = Lo; E <= Hi; E += Step)
		{
			const float D2 = FMath::Abs(T(E - Step) - 2.0f * T(E) + T(E + Step));
			if (FMath::Abs(E - Ec) <= Step * 1.01f) AtCentre = FMath::Max(AtCentre, D2);
			else if (FMath::Abs(E - Ec) > 2.0f * Step) Elsewhere = FMath::Max(Elsewhere, D2);
		}
		return AtCentre / FMath::Max(Elsewhere, 1e-9f);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefLandCoverC1Test, "CamSim.Thermal.Reference.LandCoverBlendIsC1",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefLandCoverC1Test::RunTest(const FString& Parameters)
{
	FThermalFrameParams P = MakeLandCoverParams(Down, 64, 36, 8, 10.0f, 0.0f);
	P.LandCoverCamOffsetM = FVector2f::ZeroVector;
	P.LandCoverWarpAmpM = 0.0f;
	TArray<uint8> Codes = Uniform(8, 30);                      // vegetation (296 K) ...
	for (int32 Y = 0; Y < 8; ++Y) Codes[Y * 8 + 3] = 50;        // ... with one built-up (307 K) column: x = 3, centre E = -5 m
	// Fine steps across the column's centre (texel centres at E = -15, -5, +5 m), off the row centres (N = 2 m).
	const float Step = 0.1f;
	const float Smooth = CreaseRatio([&](float E) { return TempAt(P, Codes, E, 2.0f); }, -5.0f, -14.0f, 4.0f, Step);
	// The 4A..Task 10 bilinear weights on the same line, rebuilt here from the raw fraction: a tent with a crease at the centre.
	auto Bilinear = [&](float E)
	{
		const float X = E / P.LandCoverTexelM + 4.0f - 0.5f;
		const float X0 = FMath::FloorToFloat(X), Fx = X - X0;
		auto Col = [&](float I) { return (static_cast<int32>(I) == 3) ? 307.0f : 296.0f; };
		return Col(X0) * (1.0f - Fx) + Col(X0 + 1.0f) * Fx;
	};
	const float Tent = CreaseRatio(Bilinear, -5.0f, -14.0f, 4.0f, Step);
	AddInfo(FString::Printf(TEXT("crease ratio: smoothstep %.2f, bilinear %.1f"), Smooth, Tent));
	TestTrue(TEXT("bilinear (Task 10) creases at the texel centre (the test can see a crease)"), Tent > 10.0f);
	TestTrue(TEXT("smoothstep: no crease at the texel centre (max |d2| there <= 2x elsewhere)"), Smooth <= 2.0f);
	TestNearlyEqual(TEXT("still exact at the texel centre"), TempAt(P, Codes, -5.0f, 5.0f), 307.0f, 1e-3f);
	TestNearlyEqual(TEXT("still exact at a neighbour centre"), TempAt(P, Codes, 5.0f, 5.0f), 296.0f, 1e-3f);
	int32 Bad = 0;
	for (float E = -34.0f; E <= 34.0f; E += 0.37f)
	{
		for (float N = -34.0f; N <= 34.0f; N += 0.53f)
		{
			const FLandCoverSample L = CamSimThermalRef::SampleLandCover(P, Codes.GetData(), GroundPoint(P, E, N, -700.0f));
			const float Sum = L.Weights[0] + L.Weights[1] + L.Weights[2] + L.Weights[3];
			bool bOk = L.bInside && FMath::Abs(Sum - 1.0f) <= 1e-5f;
			for (const float W : L.Weights) bOk &= (W >= 0.0f && W <= 1.0f);
			Bad += bOk ? 0 : 1;
		}
	}
	TestEqual(TEXT("weights sum to 1 and stay in [0, 1]"), Bad, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefLandCoverWarpAnchorTest, "CamSim.Thermal.Reference.LandCoverWarpGroundAnchored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefLandCoverWarpAnchorTest::RunTest(const FString& Parameters)
{
	FThermalFrameParams A = MakeLandCoverParams(Down, 64, 36, 2048, 10.0f, 30.0f);
	A.LandCoverWarpAmpM = 6.0f;
	A.LandCoverWarpCellM = 20.0f;
	A.LandCoverAnchorM = FVector2f(1234.5f, -876.25f);   // this window's centre from the session anchor
	FThermalFrameParams B = A;                           // the next window, re-centred 4.1 km east and 2.3 km south
	const FVector2f Shift(4100.0f, -2300.0f);
	B.LandCoverAnchorM = A.LandCoverAnchorM + Shift;
	B.LandCoverCamOffsetM = A.LandCoverCamOffsetM - Shift;
	int32 Moved = 0, BadAnchor = 0, BadAmp = 0, BadDet = 0, BadSmooth = 0;
	float WorstAnchor = 0.0f;
	for (float E = -3000.0f; E <= 3000.0f; E += 137.3f)
	{
		for (float N = -3000.0f; N <= 3000.0f; N += 211.7f)
		{
			const FVector2f Wa = CamSimThermalRef::WarpLandCoverEN(A, E, N);
			const FVector2f Wb = CamSimThermalRef::WarpLandCoverEN(B, E - Shift.X, N - Shift.Y);   // the same ground point
			const float D = FVector2f::Distance(Wa + A.LandCoverAnchorM, Wb + B.LandCoverAnchorM);
			WorstAnchor = FMath::Max(WorstAnchor, D);
			BadAnchor += (D <= 1e-3f) ? 0 : 1;
			const FVector2f Off = Wa - FVector2f(E, N);
			BadAmp += (FMath::Abs(Off.X) <= 6.0f + 1e-3f && FMath::Abs(Off.Y) <= 6.0f + 1e-3f) ? 0 : 1;
			Moved += (Off.Size() > 1.0f) ? 1 : 0;
			const FVector2f Again = CamSimThermalRef::WarpLandCoverEN(A, E, N);
			BadDet += (Again.X == Wa.X && Again.Y == Wa.Y) ? 0 : 1;
			// Smooth: |d offset / dG| <= amplitude * 2 * 1.5 / cell = 0.9 per axis (corner span 2, smoothstep slope <= 1.5), so
			// 10 cm on the ground moves each warp component by <= 9 cm.
			const FVector2f Near = CamSimThermalRef::WarpLandCoverEN(A, E + 0.1f, N) - FVector2f(E + 0.1f, N);
			BadSmooth += (FMath::Abs(Near.X - Off.X) <= 0.0901f && FMath::Abs(Near.Y - Off.Y) <= 0.0901f) ? 0 : 1;
		}
	}
	AddInfo(FString::Printf(TEXT("worst ground mismatch across the re-centre %.2e m"), WorstAnchor));
	TestEqual(TEXT("ground-anchored: one ground point, two window centres -> the same warped point (1e-3 m)"), BadAnchor, 0);
	TestEqual(TEXT("|offset| <= warp_amplitude_m per axis"), BadAmp, 0);
	TestEqual(TEXT("deterministic"), BadDet, 0);
	TestEqual(TEXT("smooth"), BadSmooth, 0);
	TestTrue(TEXT("the warp moves most points by more than 1 m"), Moved > 1000);
	// Two fields: East and North offsets are not the same noise.
	int32 Same = 0;
	for (float E = 0.0f; E < 400.0f; E += 7.0f) { const FVector2f O = CamSimThermalRef::WarpLandCoverEN(A, E, 3.0f) - FVector2f(E, 3.0f); Same += FMath::Abs(O.X - O.Y) < 1e-3f ? 1 : 0; }
	TestTrue(TEXT("independent East / North fields"), Same < 5);
	FThermalFrameParams Z = A;
	Z.LandCoverWarpAmpM = 0.0f;
	const FVector2f Id = CamSimThermalRef::WarpLandCoverEN(Z, 123.25f, -45.5f);
	TestTrue(TEXT("warp_amplitude_m 0: identity, exactly"), Id.X == 123.25f && Id.Y == -45.5f);
	Z = A;
	Z.LandCoverAnchorM.X = std::numeric_limits<float>::quiet_NaN();
	const FVector2f Nan = CamSimThermalRef::WarpLandCoverEN(Z, 1.0f, 2.0f);
	TestFalse(TEXT("NaN anchor: never a finite position (the lookup rejects it)"), FMath::IsFinite(Nan.X) && FMath::IsFinite(Nan.Y));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefLandCoverWarpOffTest, "CamSim.Thermal.Reference.LandCoverWarpOffIsSmoothstep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefLandCoverWarpOffTest::RunTest(const FString& Parameters)
{
	FThermalFrameParams P = MakeLandCoverParams(Down, 64, 36, 8, 10.0f, 30.0f);
	P.LandCoverWarpAmpM = 0.0f;
	P.LandCoverAnchorM = FVector2f(500.0f, 700.0f);   // irrelevant with the warp off
	const TArray<uint8> Codes = MakeLandCoverCodes(8);
	int32 Bad = 0, Inside = 0;
	for (float E = -36.0f; E <= 36.0f; E += 1.37f)
	{
		for (float N = -36.0f; N <= 36.0f; N += 1.91f)
		{
			const FVector3f Pw = GroundPoint(P, E, N, -700.0f);
			const FLandCoverSample L = CamSimThermalRef::SampleLandCover(P, Codes.GetData(), Pw);
			// The Task 7 lookup with smoothstep fractions, written out.
			const float Ex = FVector3f::DotProduct(Pw, P.LandCoverEast) / 100.0f + P.LandCoverCamOffsetM.X;
			const float Nx = FVector3f::DotProduct(Pw, P.LandCoverNorth) / 100.0f + P.LandCoverCamOffsetM.Y;
			const float X = Ex / P.LandCoverTexelM + 4.0f - 0.5f, Y = 4.0f - Nx / P.LandCoverTexelM - 0.5f;
			const bool bIn = X >= 0.0f && X <= 7.0f && Y >= 0.0f && Y <= 7.0f;
			if (L.bInside != bIn) { ++Bad; continue; }
			if (!bIn) continue;
			++Inside;
			const float X0 = FMath::Min(FMath::FloorToFloat(X), 6.0f), Y0 = FMath::Min(FMath::FloorToFloat(Y), 6.0f);
			const float Fx = X - X0, Fy = Y - Y0;
			const float Sx = Fx * Fx * (3.0f - 2.0f * Fx), Sy = Fy * Fy * (3.0f - 2.0f * Fy);
			const float W[4] = { (1.0f - Sx) * (1.0f - Sy), Sx * (1.0f - Sy), (1.0f - Sx) * Sy, Sx * Sy };
			for (int32 K = 0; K < 4; ++K) Bad += (L.Weights[K] == W[K]) ? 0 : 1;
		}
	}
	TestTrue(TEXT("samples inside the window"), Inside > 500);
	TestEqual(TEXT("warp_amplitude_m 0: exactly the smoothstep-bilinear lookup"), Bad, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalRefLandCoverWarpEdgeTest, "CamSim.Thermal.Reference.LandCoverWarpOutsideGrid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalRefLandCoverWarpEdgeTest::RunTest(const FString& Parameters)
{
	FThermalFrameParams P = MakeLandCoverParams(Down, 64, 36, 8, 10.0f, 0.0f);   // texel-centre grid E, N in [-35, 35] m
	P.LandCoverCamOffsetM = FVector2f::ZeroVector;
	P.LandCoverWarpAmpM = 6.0f;
	P.LandCoverWarpCellM = 20.0f;
	P.LandCoverAnchorM = FVector2f(-321.0f, 77.0f);
	const TArray<uint8> Codes = MakeLandCoverCodes(8);
	int32 PushedOut = 0, PulledIn = 0, Bad = 0;
	FVector2f OutPoint(0.0f), InPoint(0.0f);
	for (float E = -45.0f; E <= 45.0f; E += 0.25f)
	{
		for (float N = -45.0f; N <= 45.0f; N += 0.25f)
		{
			const bool bRawIn = FMath::Abs(E) <= 35.0f && FMath::Abs(N) <= 35.0f;
			const FVector2f W = CamSimThermalRef::WarpLandCoverEN(P, E, N);
			const bool bWarpIn = FMath::Abs(W.X) <= 34.999f && FMath::Abs(W.Y) <= 34.999f;
			const bool bWarpOut = FMath::Abs(W.X) >= 35.001f || FMath::Abs(W.Y) >= 35.001f;
			const FLandCoverSample L = CamSimThermalRef::SampleLandCover(P, Codes.GetData(), GroundPoint(P, E, N, -700.0f));
			if (bWarpOut && L.bInside) ++Bad;
			if (bWarpIn && !L.bInside) ++Bad;
			if (bRawIn && bWarpOut && PushedOut++ == 0) OutPoint = FVector2f(E, N);
			if (!bRawIn && bWarpIn && PulledIn++ == 0) InPoint = FVector2f(E, N);
		}
	}
	TestTrue(TEXT("some grid points are warped off the texel-centre grid"), PushedOut > 0);
	TestTrue(TEXT("some outside points are warped onto it"), PulledIn > 0);
	TestEqual(TEXT("inside / outside follows the warped position"), Bad, 0);
	// A terrain pixel whose warped position left the grid: terrain_default, exactly as an unwarped outside point.
	P.KFastScale = 0.0f;
	FPixelSample S;
	S.U = 0.5f; S.V = 0.6f;
	S.DeviceZ = DeviceZForPlane(P, Down, S.U, S.V, -700.0f);
	S.LandCover = Codes.GetData();
	const FVector3f Pw = CamSimThermalRef::ClipToWorld(P, S.U * 2.0f - 1.0f, 1.0f - S.V * 2.0f, S.DeviceZ);
	const float Ec = FVector3f::DotProduct(Pw, P.LandCoverEast) / 100.0f, Nc = FVector3f::DotProduct(Pw, P.LandCoverNorth) / 100.0f;
	P.LandCoverCamOffsetM = OutPoint - FVector2f(Ec, Nc);   // put the pixel on the pushed-out ground point
	const FPixelResult R = CamSimThermalRef::EvaluatePixel(P, S);
	TestFalse(TEXT("warped off the grid: no land cover"), R.bLandCover);
	TestEqual(TEXT("warped off the grid: terrain_default"), R.TempK, P.ClassTempK[P.TerrainClass]);
	P.LandCoverCamOffsetM = InPoint - FVector2f(Ec, Nc);
	TestTrue(TEXT("warped onto the grid: land cover"), CamSimThermalRef::EvaluatePixel(P, S).bLandCover);
	return true;
}
