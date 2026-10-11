// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Async/ParallelFor.h"
#include "Tests/LandCoverTestTiles.h"
#include "Thermal/LandCoverLookup.h"
#include "Vegetation/VegetationLandCover.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLandCoverMatchesLookupTest, "CamSim.Vegetation.LandCover.MatchesCodeLookup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLandCoverMatchesLookupTest::RunTest(const FString& Parameters)
{
	using namespace CamSimLandCoverTest;
	const FString Dir = TempDir(TEXT("VegSampler"));
	WriteTileDir(Dir, { FTestTile{ 666, -2350, PatternCodes() } });
	FVegetationLandCover V(Dir);
	FLandCoverCodeLookup L;
	L.Configure(Dir);
	TestTrue(TEXT("valid"), V.IsValid());
	int32 Mismatch = 0;
	for (int32 K = 0; K < 500; ++K)
	{
		L.BeginTick();
		const double Lat = 33.30 + 0.05 * ((K * 37) % 500) / 500.0, Lon = -117.50 + 0.05 * ((K * 91) % 500) / 500.0;
		Mismatch += V.Code(Lat, Lon) != L.Code(Lat, Lon);
	}
	TestEqual(TEXT("same code as the CIGI lookup (same cell pick)"), Mismatch, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLandCoverMissingTest, "CamSim.Vegetation.LandCover.MissingTilesGiveZero",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLandCoverMissingTest::RunTest(const FString& Parameters)
{
	using namespace CamSimLandCoverTest;
	const FString Dir = TempDir(TEXT("VegSamplerMissing"));
	WriteTileDir(Dir, { FTestTile{ 666, -2350, PatternCodes() } });
	// An index entry whose file is an LFS pointer.
	FFileHelper::SaveArrayToFile(LfsPointerBytes(), *FPaths::Combine(Dir, TileFile(666, -2350)));
	FVegetationLandCover V(Dir);
	TestEqual(TEXT("LFS pointer tile -> 0"), static_cast<int32>(V.Code(33.32, -117.47)), 0);
	TestEqual(TEXT("no tile -> 0"), static_cast<int32>(V.Code(10.0, 10.0)), 0);
	TestEqual(TEXT("NaN -> 0"), static_cast<int32>(V.Code(std::numeric_limits<double>::quiet_NaN(), 0.0)), 0);
	for (int32 K = 0; K < 50; ++K) V.Code(33.32, -117.47);
	const TArray<FString> W = V.TakeWarnings();
	TestTrue(TEXT("at most one summary warning"), W.Num() <= 1);
	TestEqual(TEXT("no repeat warning"), V.TakeWarnings().Num(), 0);
	FVegetationLandCover Bad(TempDir(TEXT("VegSamplerNoIndex")));
	TestFalse(TEXT("dir without index.json is invalid"), Bad.IsValid());
	TestEqual(TEXT("invalid -> 0"), static_cast<int32>(Bad.Code(33.32, -117.47)), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegLandCoverThreadsTest, "CamSim.Vegetation.LandCover.AnyThread",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegLandCoverThreadsTest::RunTest(const FString& Parameters)
{
	using namespace CamSimLandCoverTest;
	const FString Dir = TempDir(TEXT("VegSamplerThreads"));
	WriteTileDir(Dir, { FTestTile{ 666, -2350, PatternCodes() }, FTestTile{ 666, -2349, PatternCodes() } });
	FVegetationLandCover V(Dir);
	TArray<uint8> Codes; Codes.SetNumZeroed(4000);
	ParallelFor(Codes.Num(), [&](int32 K) { Codes[K] = V.Code(33.30 + 0.0001 * (K % 400), -117.50 + 0.0001 * K * 0.025); });
	int32 Mismatch = 0;
	for (int32 K = 0; K < Codes.Num(); ++K) Mismatch += Codes[K] != V.Code(33.30 + 0.0001 * (K % 400), -117.50 + 0.0001 * K * 0.025);
	TestEqual(TEXT("parallel == serial"), Mismatch, 0);
	return true;
}
