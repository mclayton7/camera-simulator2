// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Tests/LandCoverTestTiles.h"
#include "Vegetation/VegetationCanopy.h"

#include <limits>

namespace
{
	/** canopy/{cover,height} under a fresh directory: tile (666, -2350) = lat 33.30..33.35, lon -117.50..-117.45; cover 40 %
	 *  with texel (0, 0) (the north-west corner) 100 %, height 12 m with texel (0, 0) 255 m. */
	FString MakeCanopy(const TCHAR* Name, const TCHAR* Format = TEXT("camsim-canopy-1"))
	{
		using namespace CamSimLandCoverTest;
		const FString Dir = TempDir(Name);
		FTestTile Cover = Uniform(666, -2350, 40), Height = Uniform(666, -2350, 12);
		Cover.Codes[0] = 100;
		Height.Codes[0] = 255;
		Cover.Codes[1] = 255;   // texel (0, 1): no CHM pixel (no data)
		IFileManager::Get().MakeDirectory(*(Dir / TEXT("cover")), true);
		IFileManager::Get().MakeDirectory(*(Dir / TEXT("height")), true);
		WriteTileDir(Dir / TEXT("cover"), { Cover }, Format);
		WriteTileDir(Dir / TEXT("height"), { Height }, Format);
		return Dir;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegCanopySampleTest, "CamSim.Vegetation.Canopy.Sample",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegCanopySampleTest::RunTest(const FString& Parameters)
{
	FVegetationCanopy C(MakeCanopy(TEXT("CanopySample")));
	if (!TestTrue(*FString::Printf(TEXT("valid (%s)"), *C.GetError()), C.IsValid())) return true;
	const double Half = 0.5 / 12000.0;
	const TOptional<FCanopySample> Corner = C.Sample(33.35 - Half, -117.50 + Half);
	if (TestTrue(TEXT("corner texel set"), Corner.IsSet()))
	{
		TestEqual(TEXT("corner cover 1"), Corner->CoverFrac, 1.0f);
		TestEqual(TEXT("corner height 255"), Corner->HeightM, 255.0f);
	}
	const TOptional<FCanopySample> In = C.Sample(33.32, -117.47);
	if (TestTrue(TEXT("inside set"), In.IsSet()))
	{
		TestEqual(TEXT("cover 0.4"), In->CoverFrac, 0.4f);
		TestEqual(TEXT("height 12"), In->HeightM, 12.0f);
	}
	TestFalse(TEXT("neighbouring tile (not in the layer): unset"), C.Sample(33.32, -117.42).IsSet());
	TestFalse(TEXT("no-data texel (cover 255): unset, so the R4 rule applies"), C.Sample(33.35 - Half, -117.50 + 3.0 * Half).IsSet());
	TestFalse(TEXT("NaN: unset"), C.Sample(std::numeric_limits<double>::quiet_NaN(), -117.47).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegCanopyInvalidTest, "CamSim.Vegetation.Canopy.InvalidIndex",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegCanopyInvalidTest::RunTest(const FString& Parameters)
{
	FVegetationCanopy Wrong(MakeCanopy(TEXT("CanopyWrongFormat"), TEXT("camsim-landcover-1")));
	TestFalse(TEXT("a land-cover index is not a canopy index"), Wrong.IsValid());
	TestTrue(TEXT("error names the format"), Wrong.GetError().Contains(TEXT("format")));
	TestFalse(TEXT("invalid: unset"), Wrong.Sample(33.32, -117.47).IsSet());
	FVegetationCanopy Missing(CamSimLandCoverTest::TempDir(TEXT("CanopyNone")));
	TestFalse(TEXT("no indexes: invalid"), Missing.IsValid());
	// The default land-cover cache refuses a canopy index.
	FLandCoverTileCache LandCover(MakeCanopy(TEXT("CanopyAsLandCover")) / TEXT("cover"));
	TestFalse(TEXT("land-cover cache refuses camsim-canopy-1"), LandCover.IsValid());
	return true;
}

#include "Vegetation/VegetationManager.h"
#include "Vegetation/VegetationScatter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegCanopyFallbackTest, "CamSim.Vegetation.Canopy.OutsideTheLayerUsesTheLandCoverRule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegCanopyFallbackTest::RunTest(const FString& Parameters)
{
	using namespace CamSimVegetation;
	FVegetationCanopy C(MakeCanopy(TEXT("CanopyFallback")));
	const FVegPoint Inside = CombinePoint(10, C.Sample(33.32, -117.47));
	TestTrue(TEXT("inside: canopy"), Inside.bHasCanopy);
	TestEqual(TEXT("inside: cover"), Inside.CoverFrac, 0.4f);
	TestEqual(TEXT("code kept"), int32(Inside.Code), 10);
	const FVegPoint Outside = CombinePoint(10, C.Sample(33.32, -117.42));
	TestFalse(TEXT("outside: no canopy (R4 rule)"), Outside.bHasCanopy);
	TestEqual(TEXT("outside: code kept"), int32(Outside.Code), 10);
	// A cell straddling the layer's east edge (lon -117.45): trees on both sides, R4's rule east of it.
	FScatterParams P; P.bShrubs = false;
	const FVegetationMix Mix = SingleTypeMix(10, 0, 9.0f, 2.7f);
	auto Sample = [&C](double La, double Lo) { return CombinePoint(10, C.Sample(La, Lo)); };
	const FVegCellId Edge = CellOf(33.32, -117.45, P.CellSizeM);
	int32 West = 0, East = 0;
	for (int32 K = -1; K <= 1; ++K)
		for (const FTreeInstance& T : GenerateCellContent(VegetationSeed(4u), { Edge.Row + K, Edge.Col }, P, Mix, Sample).Trees)
			(T.LonDeg < -117.45 ? West : East)++;
	TestTrue(TEXT("trees west of the edge (canopy 40 %)"), West > 0);
	TestTrue(TEXT("trees east of the edge (land-cover rule, no gap)"), East > 0);
	return true;
}
