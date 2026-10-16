// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "GroundTruth/SemanticClasses.h"
#include "GroundTruth/SemanticFrame.h"
#include "GroundTruth/SemanticReference.h"
#include "Thermal/LandCoverGeometry.h"
#include "Thermal/LandCoverWindow.h"
#include "Thermal/ThermalFrameBuilder.h"

// -------------------------------------------------------------------------
// Semantic class per texel (ROADMAP 4D): the CPU reference InstanceIdCS's
// WRITE_SEMANTIC mirrors, and the frame-params builder.
// -------------------------------------------------------------------------

using namespace CamSimSemantic;

namespace
{
	/** A 4 x 4 window of 10 m texels centred on the camera, East = +X, North = +Y (world = translated world here). */
	FSemanticFrameParams Window4(const TArray<uint8>& Codes)
	{
		FSemanticFrameParams P;
		P.bLandCover = 1;
		P.LandCoverWindowId = 1;
		P.LandCoverEast = FVector3f(1, 0, 0);
		P.LandCoverNorth = FVector3f(0, 1, 0);
		P.LandCoverCamOffsetM = FVector2f(0, 0);
		P.LandCoverTexelM = 10.0f;
		P.LandCoverTexels = 4;
		const TStaticArray<uint8, 256> Lut = BuildCodeLut();
		for (int32 I = 0; I < 256; ++I) P.CodeClass[I] = Lut[I];
		(void)Codes;
		return P;
	}

	struct FFixture
	{
		TArray<uint8> Codes;   // 4 x 4, row 0 = north
		FSemanticFrameParams P;
		explicit FFixture(TArray<uint8> InCodes) : Codes(MoveTemp(InCodes)), P(Window4(Codes)) {}
		uint8 At(float SceneZ, float CustomZ, uint32 Stencil, const FVector3f& Pw) const
		{
			return CamSimSemanticRef::ClassifyTexel(P, SceneZ, CustomZ, Stencil, Pw,
				[this](int32 X, int32 Y) { return Codes[Y * 4 + X]; });
		}
	};

	// Texel (x, y) = floor(E/10 + 2), floor(2 - N/10): (E, N) = (-15, 15) -> (0, 0); (15, -15) -> (3, 3)
	TArray<uint8> Codes16()
	{
		return { 10, 20, 50, 60,
		         70, 80, 200, 201,
		         210, 220, 230, 90,
		         0, 30, 40, 95 };
	}
	FVector3f AtTexel(int32 X, int32 Y, float UpCm = -3000.0f)
	{
		return FVector3f((X - 2 + 0.5f) * 1000.0f, (2 - Y - 0.5f) * 1000.0f, UpCm);   // cm, texel centre
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSemanticRefSkyTest, "CamSim.GroundTruth.Semantic.Ref.Sky",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSemanticRefSkyTest::RunTest(const FString& Parameters)
{
	const FFixture F(Codes16());
	TestEqual(TEXT("far plane"), F.At(0.0f, 0.0f, 0, AtTexel(1, 1)), static_cast<uint8>(Sky));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSemanticRefEntityTest, "CamSim.GroundTruth.Semantic.Ref.Entities",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSemanticRefEntityTest::RunTest(const FString& Parameters)
{
	FFixture F(Codes16());
	F.P.StencilClass[5] = Vessel;
	TestEqual(TEXT("visible entity"), F.At(0.5f, 0.5f, 5, AtTexel(0, 0)), static_cast<uint8>(Vessel));
	TestEqual(TEXT("hidden entity takes the occluder's (terrain) class"), F.At(0.6f, 0.5f, 5, AtTexel(0, 0)), static_cast<uint8>(Tree));
	TestEqual(TEXT("stencil without a class falls through to terrain"), F.At(0.5f, 0.5f, 9, AtTexel(0, 0)), static_cast<uint8>(Tree));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSemanticRefCodesTest, "CamSim.GroundTruth.Semantic.Ref.EachCode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSemanticRefCodesTest::RunTest(const FString& Parameters)
{
	FFixture F(Codes16());
	F.P.bSea = 0;
	const TStaticArray<uint8, 256> Lut = BuildCodeLut();
	for (int32 Y = 0; Y < 4; ++Y)
	{
		for (int32 X = 0; X < 4; ++X)
		{
			const uint8 Code = F.Codes[Y * 4 + X];
			TestEqual(FString::Printf(TEXT("texel (%d, %d) code %u"), X, Y, Code), F.At(0.5f, 0.0f, 0, AtTexel(X, Y)), Lut[Code]);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSemanticRefNearestTest, "CamSim.GroundTruth.Semantic.Ref.NearestTexelBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSemanticRefNearestTest::RunTest(const FString& Parameters)
{
	const FFixture F(Codes16());
	int32 X = -1, Y = -1;
	TestTrue(TEXT("inside"), CamSimSemanticRef::NearestLandCoverTexel(F.P, FVector3f(-999.0f, 0.0f, 0.0f), X, Y));
	TestEqual(TEXT("E -9.99 m: texel 1"), X, 1);
	TestTrue(TEXT("inside"), CamSimSemanticRef::NearestLandCoverTexel(F.P, FVector3f(1.0f, 0.0f, 0.0f), X, Y));
	TestEqual(TEXT("E +0.01 m: texel 2"), X, 2);
	TestEqual(TEXT("N +0.0 m: row 2"), Y, 2);
	TestTrue(TEXT("edge, inside"), CamSimSemanticRef::NearestLandCoverTexel(F.P, FVector3f(1999.0f, 0.0f, 0.0f), X, Y));
	TestEqual(TEXT("E 19.99 m: last column"), X, 3);
	TestFalse(TEXT("E 20.01 m: outside"), CamSimSemanticRef::NearestLandCoverTexel(F.P, FVector3f(2001.0f, 0.0f, 0.0f), X, Y));
	TestFalse(TEXT("NaN: outside"), CamSimSemanticRef::NearestLandCoverTexel(F.P, FVector3f(NAN, 0.0f, 0.0f), X, Y));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSemanticRefOutsideTest, "CamSim.GroundTruth.Semantic.Ref.NoWindowUnlabeled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSemanticRefOutsideTest::RunTest(const FString& Parameters)
{
	FFixture F(Codes16());
	TestEqual(TEXT("outside the window"), F.At(0.5f, 0.0f, 0, FVector3f(5000.0f, 0.0f, -3000.0f)), static_cast<uint8>(Unlabeled));
	F.P.bLandCover = 0;
	TestEqual(TEXT("no window: unlabeled, never a stale class"), F.At(0.5f, 0.0f, 0, AtTexel(0, 0)), static_cast<uint8>(Unlabeled));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSemanticRefSeaTest, "CamSim.GroundTruth.Semantic.Ref.SeaBand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSemanticRefSeaTest::RunTest(const FString& Parameters)
{
	FFixture F(Codes16());
	F.P.bSea = 1;
	F.P.Up = FVector3f(0, 0, 1);
	F.P.CamHeightCm = 3000.0f;      // camera 30 m above sea level
	F.P.SeaRadiusCm = 6.371e8f;
	F.P.WaterBandCm = 50.0f;
	// Code 0 at texel (0, 3); a point at sea level there is water, one 2 m above is not
	TestEqual(TEXT("sea level, code 0: water"), F.At(0.5f, 0.0f, 0, AtTexel(0, 3, -3000.0f)), static_cast<uint8>(Water));
	TestEqual(TEXT("2 m above the band, code 0: unlabeled"), F.At(0.5f, 0.0f, 0, AtTexel(0, 3, -2800.0f)), static_cast<uint8>(Unlabeled));
	TestEqual(TEXT("outside the window at sea level: water"), F.At(0.5f, 0.0f, 0, FVector3f(9000.0f, 0.0f, -3000.0f)), static_cast<uint8>(Water));
	TestEqual(TEXT("in the band on a land code (tree): keeps the land class"), F.At(0.5f, 0.0f, 0, AtTexel(0, 0, -3000.0f)), static_cast<uint8>(Tree));
	F.P.bSea = 0;
	TestEqual(TEXT("no ocean: no band"), F.At(0.5f, 0.0f, 0, FVector3f(9000.0f, 0.0f, -3000.0f)), static_cast<uint8>(Unlabeled));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSemanticBuilderTest, "CamSim.GroundTruth.Semantic.Builder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSemanticBuilderTest::RunTest(const FString& Parameters)
{
	FLandCoverWindowData W;
	W.Id = 7;
	W.Spec.CentreLatDeg = 33.3;
	W.Spec.CentreLonDeg = -117.3;
	W.Spec.Texels = 2048;
	W.Spec.TexelM = 10.0;
	W.NonZeroTexels = 100;
	FSemanticBuildInputs In;
	In.Window = &W;
	In.EastWorld = FVector(1, 0, 0);
	In.NorthWorld = FVector(0, -1, 0);
	In.UpWorld = FVector(0, 0, 1);
	In.CamLatDeg = 33.31;
	In.CamLonDeg = -117.29;
	In.CamAltHaeM = 500.0;
	In.SeaLevelHaeM = -35.0;
	In.MaxWaveAmplitudeM = 1.25;
	In.StencilClass = TStaticArray<uint8, 256>(InPlace, static_cast<uint8>(0));
	In.StencilClass[4] = Vehicle;
	const FSemanticFrameParams P = BuildFrameParams(In);
	TestEqual(TEXT("land cover on"), P.bLandCover, 1u);
	TestEqual(TEXT("window id"), P.LandCoverWindowId, 7u);
	const FVector2D Off = CamSimLandCover::GeodeticToWindowEN(W.Spec, In.CamLatDeg, In.CamLonDeg);
	TestTrue(TEXT("camera offset = GeodeticToWindowEN"), FMath::IsNearlyEqual(P.LandCoverCamOffsetM.X, static_cast<float>(Off.X), 1e-3f)
		&& FMath::IsNearlyEqual(P.LandCoverCamOffsetM.Y, static_cast<float>(Off.Y), 1e-3f));
	TestEqual(TEXT("texels"), P.LandCoverTexels, 2048u);
	TestEqual(TEXT("north axis"), P.LandCoverNorth, FVector3f(0, -1, 0));
	TestEqual(TEXT("sea on"), P.bSea, 1u);
	TestEqual(TEXT("camera height above sea (cm), as ThermalCS"), P.CamHeightCm, 53500.0f);
	TestEqual(TEXT("sea radius, as ThermalCS"), P.SeaRadiusCm, static_cast<float>(FThermalFrameBuilder::GaussianRadiusM(In.CamLatDeg) * 100.0));
	TestEqual(TEXT("band, as ThermalCS"), P.WaterBandCm, static_cast<float>((FThermalFrameBuilder::WaterBandBaseM + 1.25) * 100.0));
	TestEqual(TEXT("code LUT"), P.CodeClass[200], static_cast<uint8>(Road));
	TestEqual(TEXT("stencil table"), P.StencilClass[4], static_cast<uint8>(Vehicle));

	FSemanticBuildInputs None = In;
	None.Window = nullptr;
	None.SeaLevelHaeM.Reset();
	const FSemanticFrameParams Q = BuildFrameParams(None);
	TestEqual(TEXT("no window: land cover off"), Q.bLandCover, 0u);
	TestEqual(TEXT("no sea level: no band"), Q.bSea, 0u);
	FLandCoverWindowData Empty = W;
	Empty.NonZeroTexels = 0;
	None.Window = &Empty;
	TestEqual(TEXT("an all-zero window counts as none"), BuildFrameParams(None).bLandCover, 0u);
	return true;
}
