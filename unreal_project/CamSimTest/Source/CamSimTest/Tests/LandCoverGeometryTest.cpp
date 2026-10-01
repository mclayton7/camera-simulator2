// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/LandCoverGeometry.h"
#include "Thermal/LandCoverTiles.h"

// CamSim.Thermal.LandCover.*: window geometry and resample against analytic patterns (ROADMAP 4B).

using namespace CamSimLandCover;

namespace
{
	constexpr int32 Px = FLandCoverTileCache::TilePx;

	FWindowSpec Spec(double Lat, double Lon, int32 Texels, float TexelM = 10.0f)
	{
		FWindowSpec S;
		S.CentreLatDeg = Lat; S.CentreLonDeg = Lon; S.Texels = Texels; S.TexelM = TexelM;
		return S;
	}

	TArray<uint8> TileOf(TFunctionRef<uint8(int32 Col, int32 Row)> Code)
	{
		TArray<uint8> T;
		T.SetNumUninitialized(Px * Px);
		for (int32 R = 0; R < Px; ++R)
			for (int32 C = 0; C < Px; ++C) T[R * Px + C] = Code(C, R);
		return T;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverMappingTest, "CamSim.Thermal.LandCover.SmallAreaMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverMappingTest::RunTest(const FString& Parameters)
{
	TestNearlyEqual(TEXT("M at the equator = a (1 - e^2)"), MeridionalRadiusM(0.0), 6335439.327, 0.01);
	TestNearlyEqual(TEXT("N at the equator = a"), PrimeVerticalRadiusM(0.0), 6378137.0, 0.01);
	TestTrue(TEXT("M < N at 37.8"), MeridionalRadiusM(37.8) < PrimeVerticalRadiusM(37.8));
	const FWindowSpec W = Spec(37.7752, -122.4750, 2048);
	for (const FVector2D EN : { FVector2D(0, 0), FVector2D(5000, -3000), FVector2D(-10240, 10240) })
	{
		double Lat = 0.0, Lon = 0.0;
		WindowENToGeodetic(W, EN.X, EN.Y, Lat, Lon);
		const FVector2D Back = GeodeticToWindowEN(W, Lat, Lon);
		TestNearlyEqual(*FString::Printf(TEXT("E round trip %.0f"), EN.X), Back.X, EN.X, 1e-6);
		TestNearlyEqual(*FString::Printf(TEXT("N round trip %.0f"), EN.Y), Back.Y, EN.Y, 1e-6);
	}
	double Lat = 0.0, Lon = 0.0;
	WindowENToGeodetic(W, 0.0, 1000.0, Lat, Lon);
	TestTrue(TEXT("+N is north"), Lat > W.CentreLatDeg && Lon == W.CentreLonDeg);
	WindowENToGeodetic(W, 1000.0, 0.0, Lat, Lon);
	TestTrue(TEXT("+E is east"), Lon > W.CentreLonDeg && Lat == W.CentreLatDeg);
	const FWindowSpec Dateline = Spec(0.0, 179.99, 2048);
	TestTrue(TEXT("wraps across the antimeridian"), FMath::IsNearlyEqual(GeodeticToWindowEN(Dateline, 0.0, -179.99).X,
		FMath::DegreesToRadians(0.02) * PrimeVerticalRadiusM(0.0), 1e-3));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverCellTest, "CamSim.Thermal.LandCover.GlobalCellAndTile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverCellTest::RunTest(const FString& Parameters)
{
	FIntPoint Tile, In;
	CellToTile(GlobalCell(37.80 - 1e-9, -122.50 + 1e-9), Tile, In);
	TestEqual(TEXT("NW corner tile"), Tile, FIntPoint(755, -2450));
	TestEqual(TEXT("NW corner cell"), In, FIntPoint(0, 0));
	CellToTile(GlobalCell(37.75 + 1e-9, -122.45 - 1e-9), Tile, In);
	TestEqual(TEXT("SE corner tile"), Tile, FIntPoint(755, -2450));
	TestEqual(TEXT("SE corner cell"), In, FIntPoint(Px - 1, Px - 1));
	CellToTile(GlobalCell(10.0, 179.99999), Tile, In);
	TestEqual(TEXT("last column before 180"), Tile.Y, 3599);
	CellToTile(GlobalCell(10.0, -180.0), Tile, In);
	TestEqual(TEXT("first column at -180"), Tile.Y, -3600);
	CellToTile(GlobalCell(10.0, 180.0), Tile, In);
	TestEqual(TEXT("+180 wraps to -180"), Tile.Y, -3600);
	CellToTile(GlobalCell(-89.99999, 0.0), Tile, In);
	TestEqual(TEXT("south pole row clamps"), Tile.X, -1800);
	// Negative lat/lon: floor, not truncation. Tile (-677, -1400) spans lat -33.85..-33.80, lon -70.00..-69.95.
	CellToTile(GlobalCell(-33.80 - 1e-9, -70.0 + 1e-9), Tile, In);
	TestEqual(TEXT("negative lat/lon NW corner tile"), Tile, FIntPoint(-677, -1400));
	TestEqual(TEXT("negative lat/lon NW corner cell"), In, FIntPoint(0, 0));
	CellToTile(GlobalCell(-33.85 + 1e-9, -69.95 - 1e-9), Tile, In);
	TestEqual(TEXT("negative lat/lon SE corner tile"), Tile, FIntPoint(-677, -1400));
	TestEqual(TEXT("negative lat/lon SE corner cell"), In, FIntPoint(Px - 1, Px - 1));
	CellToTile(GlobalCell(-33.80 + 1e-9, -70.0 - 1e-9), Tile, In);
	TestEqual(TEXT("just north/west of the corner is the next tile"), Tile, FIntPoint(-676, -1401));
	CellToTile(GlobalCell(-33.8, -70.0), Tile, In);   // exactly on the grid: round-off must not drop it a cell
	TestEqual(TEXT("exact negative grid corner tile"), Tile, FIntPoint(-677, -1400));
	TestEqual(TEXT("exact negative grid corner cell"), In, FIntPoint(0, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverOrientationTest, "CamSim.Thermal.LandCover.ResampleOrientationAndCentre",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverOrientationTest::RunTest(const FString& Parameters)
{
	// Tile (755, -2450) split at its middle row (37.775 N) and middle column (122.475 W); the window is centred on the split.
	const TArray<uint8> NorthSouth = TileOf([](int32 C, int32 R) { return static_cast<uint8>(R < Px / 2 ? 10 : 50); });
	const TArray<uint8> WestEast   = TileOf([](int32 C, int32 R) { return static_cast<uint8>(C < Px / 2 ? 30 : 60); });
	const FWindowSpec W = Spec(37.775, -122.475, 64);
	TArray<uint8> Out;
	auto Only = [](const TArray<uint8>& T) { return [&T](int32 Lat, int32 Lon) -> const uint8* { return (Lat == 755 && Lon == -2450) ? T.GetData() : nullptr; }; };
	TestEqual(TEXT("every texel has data"), Resample(W, Only(NorthSouth), Out), static_cast<int64>(64 * 64));
	TestEqual(TEXT("size"), Out.Num(), 64 * 64);
	int32 Bad = 0;
	for (int32 Y = 0; Y < 64; ++Y)
		for (int32 X = 0; X < 64; ++X) Bad += Out[Y * 64 + X] != (Y < 32 ? 10 : 50);
	TestEqual(TEXT("row 0 is north: rows < 32 north of the split"), Bad, 0);
	Resample(W, Only(WestEast), Out);
	Bad = 0;
	for (int32 Y = 0; Y < 64; ++Y)
		for (int32 X = 0; X < 64; ++X) Bad += Out[Y * 64 + X] != (X < 32 ? 30 : 60);
	TestEqual(TEXT("column 0 is west: columns < 32 west of the split"), Bad, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverScaleTest, "CamSim.Thermal.LandCover.ResampleScale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverScaleTest::RunTest(const FString& Parameters)
{
	// Stripes every 60 cells (0.005 deg of longitude) everywhere: boundaries at lon = k * 0.005 deg.
	const TArray<uint8> Stripes = TileOf([](int32 C, int32 R) { return static_cast<uint8>((C / 60) % 2 ? 50 : 10); });
	const FWindowSpec W = Spec(37.7752, -122.4750, 512);
	TArray<uint8> Out;
	Resample(W, [&Stripes](int32, int32) -> const uint8* { return Stripes.GetData(); }, Out);
	const int32 Row = 256;
	TArray<double> Boundaries;   // E of every stripe boundary inside the window
	for (int32 K = FMath::FloorToInt32(-122.6 / 0.005); K <= FMath::CeilToInt32(-122.35 / 0.005); ++K)
	{
		const double E = GeodeticToWindowEN(W, W.CentreLatDeg, K * 0.005).X;
		if (E > -2555.0 && E < 2555.0) Boundaries.Add(E);
	}
	int32 Transitions = 0, Misplaced = 0;
	for (int32 X = 1; X < 512; ++X)
	{
		if (Out[Row * 512 + X] == Out[Row * 512 + X - 1]) continue;
		++Transitions;
		const double Lo = (X - 1 + 0.5 - 256) * 10.0, Hi = (X + 0.5 - 256) * 10.0;
		const bool bFound = Boundaries.ContainsByPredicate([Lo, Hi](double E) { return E >= Lo - 1e-6 && E <= Hi + 1e-6; });
		Misplaced += bFound ? 0 : 1;
	}
	TestEqual(TEXT("one transition per analytic boundary (scale)"), Transitions, Boundaries.Num());
	TestEqual(TEXT("each transition between the texels its boundary falls between"), Misplaced, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverMissingTest, "CamSim.Thermal.LandCover.ResampleMissingTilesAreZero",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverMissingTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Twenty = TileOf([](int32, int32) { return static_cast<uint8>(20); });
	const FWindowSpec W = Spec(37.80, -122.475, 64);   // straddles the 755 / 756 tile row boundary
	TArray<uint8> Out;
	const int64 NonZero = Resample(W, [&Twenty](int32 Lat, int32) -> const uint8* { return Lat == 756 ? Twenty.GetData() : nullptr; }, Out);
	TestEqual(TEXT("north half present, south half missing"), NonZero, static_cast<int64>(32 * 64));
	TestEqual(TEXT("north texel"), Out[10 * 64 + 5], static_cast<uint8>(20));
	TestEqual(TEXT("missing tile -> 0"), Out[50 * 64 + 5], static_cast<uint8>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverAntimeridianTest, "CamSim.Thermal.LandCover.ResampleAntimeridian",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverAntimeridianTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> West = TileOf([](int32, int32) { return static_cast<uint8>(10); });
	const TArray<uint8> East = TileOf([](int32, int32) { return static_cast<uint8>(50); });
	const FWindowSpec W = Spec(10.025, 179.999, 64);
	TArray<uint8> Out;
	const int64 NonZero = Resample(W, [&](int32 Lat, int32 Lon) -> const uint8*
	{
		if (Lat != 200) return nullptr;
		return Lon == 3599 ? West.GetData() : (Lon == -3600 ? East.GetData() : nullptr);
	}, Out);
	TestEqual(TEXT("both sides found"), NonZero, static_cast<int64>(64 * 64));
	const double E180 = GeodeticToWindowEN(W, W.CentreLatDeg, 180.0).X;
	int32 Bad = 0;
	for (int32 X = 0; X < 64; ++X)
	{
		const double E = (X + 0.5 - 32) * 10.0;
		Bad += Out[32 * 64 + X] != (E < E180 ? 10 : 50);
	}
	TestEqual(TEXT("west of 180 from tile 3599, east from tile -3600"), Bad, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverRecentreTest, "CamSim.Thermal.LandCover.RecentreAndPole",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverRecentreTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("north pole window refused"), IsWindowAllowed(89.5));
	TestFalse(TEXT("south pole window refused"), IsWindowAllowed(-89.5));
	TestTrue(TEXT("88.9 allowed"), IsWindowAllowed(88.9));
	const FWindowSpec W = Spec(37.7, -122.4, 2048);   // 20.48 km: 25 % = 5120 m
	auto At = [&W](double E, double N) { double Lat = 0.0, Lon = 0.0; WindowENToGeodetic(W, E, N, Lat, Lon); return FVector2D(Lat, Lon); };
	TestFalse(TEXT("5000 m north: stays"), NeedsRecentre(W, At(0, 5000).X, At(0, 5000).Y, 0.25f));
	TestTrue (TEXT("5200 m north: re-centre"), NeedsRecentre(W, At(0, 5200).X, At(0, 5200).Y, 0.25f));
	TestTrue (TEXT("5200 m east: re-centre"), NeedsRecentre(W, At(5200, 0).X, At(5200, 0).Y, 0.25f));
	TestTrue (TEXT("5200 m south-west: re-centre"), NeedsRecentre(W, At(-5200, -100).X, At(-5200, -100).Y, 0.25f));
	const FWindowSpec D = Spec(0.0, 179.99, 2048);
	TestFalse(TEXT("2.2 km across the antimeridian: stays"), NeedsRecentre(D, 0.0, -179.99, 0.25f));
	TestTrue (TEXT("... but re-centres at 5 %"), NeedsRecentre(D, 0.0, -179.99, 0.05f));
	return true;
}
