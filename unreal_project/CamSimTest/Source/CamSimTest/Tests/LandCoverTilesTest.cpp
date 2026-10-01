// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Tasks/Task.h"
#include "Thermal/LandCoverTiles.h"
#include "Tests/LandCoverTestTiles.h"

// CamSim.Thermal.LandCover.*: tile index parsing, PNG decode, LRU, the committed SF sample (ROADMAP 4B).

using namespace CamSimLandCoverTest;

namespace
{
	const TCHAR* GoodIndex =
		TEXT("{\"format\":\"camsim-landcover-1\",\"tile_deg\":0.05,\"tile_px\":600,\"licence\":\"CC BY 4.0\",")
		TEXT("\"attribution\":\"(c) ESA WorldCover project 2021\",\"tiles\":[")
		TEXT("{\"file\":\"+37.75_-122.50.png\",\"lat_index\":755,\"lon_index\":-2450},")
		TEXT("{\"file\":\"+37.75_-122.45.png\",\"lat_index\":755,\"lon_index\":-2449}]}");

	TConstArrayView<uint8> View(const TArray64<uint8>& A) { return TConstArrayView<uint8>(A.GetData(), static_cast<int32>(A.Num())); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverIndexParseTest, "CamSim.Thermal.LandCover.IndexParse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverIndexParseTest::RunTest(const FString& Parameters)
{
	FLandCoverIndex Index;
	FString Err;
	const FString Good(GoodIndex);
	TestTrue(TEXT("valid index parses"), FLandCoverTileCache::ParseIndex(Good, Index, Err));
	TestEqual(TEXT("two tiles"), Index.Files.Num(), 2);
	const FString* F = Index.Files.Find(FIntPoint(755, -2450));
	TestTrue(TEXT("keyed by (lat_index, lon_index)"), F != nullptr && *F == TEXT("+37.75_-122.50.png"));
	TestEqual(TEXT("licence"), Index.Licence, FString(TEXT("CC BY 4.0")));
	TestTrue(TEXT("attribution"), Index.Attribution.Contains(TEXT("ESA WorldCover")));
	auto Bad = [this](const FString& Json, const TCHAR* Why)
	{
		FLandCoverIndex I;
		FString E;
		TestFalse(Why, FLandCoverTileCache::ParseIndex(Json, I, E));
		TestFalse(*FString::Printf(TEXT("%s: has an error message"), Why), E.IsEmpty());
	};
	Bad(TEXT("not json"), TEXT("garbage"));
	Bad(Good.Replace(TEXT("camsim-landcover-1"), TEXT("camsim-landcover-9")), TEXT("wrong format"));
	Bad(Good.Replace(TEXT("\"tile_px\":600"), TEXT("\"tile_px\":512")), TEXT("wrong tile_px"));
	Bad(Good.Replace(TEXT("\"tile_deg\":0.05"), TEXT("\"tile_deg\":0.1")), TEXT("wrong tile_deg"));
	Bad(Good.Replace(TEXT("+37.75_-122.50.png"), TEXT("../evil.png")), TEXT("path in a file name"));
	Bad(Good.Replace(TEXT("\"lat_index\":755"), TEXT("\"lat_index\":755.5")), TEXT("fractional index"));
	Bad(Good.Replace(TEXT("\"lat_index\":755"), TEXT("\"lat_index\":4000")), TEXT("index out of range"));
	Bad(Good.Replace(TEXT("\"tiles\""), TEXT("\"tilez\"")), TEXT("no tiles array"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverPngDecodeTest, "CamSim.Thermal.LandCover.PngDecode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverPngDecodeTest::RunTest(const FString& Parameters)
{
	IImageWrapperModule& M = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const TArray<uint8> Codes = PatternCodes();
	TArray<uint8> Out;
	FString Err;
	TestTrue(TEXT("decodes"), FLandCoverTileCache::DecodeTilePng(M, View(EncodeGreyPng(Codes, TilePx, TilePx)), Out, Err));
	TestTrue(TEXT("codes pass through losslessly"), Out == Codes);

	const TArray<uint8> Small(Codes.GetData(), 100 * 100);
	TestFalse(TEXT("wrong size rejected"), FLandCoverTileCache::DecodeTilePng(M, View(EncodeGreyPng(Small, 100, 100)), Out, Err));
	TestTrue(TEXT("expected size in the error"), Err.Contains(TEXT("600")));

	const TSharedPtr<IImageWrapper> W = M.CreateImageWrapper(EImageFormat::PNG);
	TArray<uint8> Rgba;
	Rgba.Init(10, TilePx * TilePx * 4);
	W->SetRaw(Rgba.GetData(), Rgba.Num(), TilePx, TilePx, ERGBFormat::RGBA, 8);
	TestFalse(TEXT("RGBA rejected"), FLandCoverTileCache::DecodeTilePng(M, View(W->GetCompressed()), Out, Err));

	TestFalse(TEXT("LFS pointer rejected"), FLandCoverTileCache::DecodeTilePng(M, LfsPointerBytes(), Out, Err));
	TestTrue(TEXT("LFS hint"), Err.Contains(TEXT("git lfs pull")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverCacheLruTest, "CamSim.Thermal.LandCover.CacheLruAndMissing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverCacheLruTest::RunTest(const FString& Parameters)
{
	const FString Dir = TempDir(TEXT("Lru"));
	TArray<FTestTile> Tiles;
	for (int32 K = 0; K < 6; ++K) Tiles.Add(Uniform(755, -2450 + K, static_cast<uint8>(10 * (K + 1))));
	WriteTileDir(Dir, Tiles);
	FLandCoverTileCache Cache(Dir, 4);
	if (!TestTrue(*FString::Printf(TEXT("valid (%s)"), *Cache.GetError()), Cache.IsValid())) return false;
	for (int32 K = 0; K < 6; ++K)
	{
		const auto T = Cache.Get(755, -2450 + K);
		if (TestTrue(*FString::Printf(TEXT("tile %d loads"), K), T.IsValid()))
		{
			TestEqual(TEXT("its code"), T->Codes[12345], static_cast<uint8>(10 * (K + 1)));
			TestEqual(TEXT("its indices"), FIntPoint(T->LatIndex, T->LonIndex), FIntPoint(755, -2450 + K));
		}
	}
	TestEqual(TEXT("LRU keeps 4"), Cache.NumCached(), 4);
	TestEqual(TEXT("6 decodes"), Cache.NumLoads(), 6);
	Cache.Get(755, -2445);   // most recent: a hit
	TestEqual(TEXT("a hit doesn't decode"), Cache.NumLoads(), 6);
	Cache.Get(755, -2450);   // evicted first: decoded again
	TestEqual(TEXT("an evicted tile decodes again"), Cache.NumLoads(), 7);
	TestFalse(TEXT("not in the index -> null"), Cache.Get(700, 0).IsValid());
	TestFalse(TEXT("HasTile"), Cache.HasTile(700, 0));
	TestEqual(TEXT("no warnings"), Cache.TakeWarnings().Num(), 0);

	FLandCoverTileCache Missing(Dir / TEXT("does_not_exist"));
	TestFalse(TEXT("missing directory -> invalid"), Missing.IsValid());
	TestTrue(TEXT("error names the index path"), Missing.GetError().Contains(TEXT("index.json")));
	TestFalse(TEXT("invalid cache returns null"), Missing.Get(755, -2450).IsValid());
	return true;
}

// Review Focus 1: a clone without `git lfs pull` (or a damaged tile) is reported once and treated as no data.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverCorruptTileTest, "CamSim.Thermal.LandCover.CorruptTileIsMissing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverCorruptTileTest::RunTest(const FString& Parameters)
{
	const FString Dir = TempDir(TEXT("Corrupt"));
	WriteTileDir(Dir, { Uniform(755, -2450, 10), Uniform(755, -2449, 50) });
	FFileHelper::SaveArrayToFile(LfsPointerBytes(), *FPaths::Combine(Dir, TileFile(755, -2450)));
	IFileManager::Get().Delete(*FPaths::Combine(Dir, TileFile(755, -2449)));
	FLandCoverTileCache Cache(Dir);
	TestTrue(TEXT("index still valid"), Cache.IsValid());
	TestFalse(TEXT("pointer file -> null"), Cache.Get(755, -2450).IsValid());
	TestFalse(TEXT("deleted file -> null"), Cache.Get(755, -2449).IsValid());
	const TArray<FString> W = Cache.TakeWarnings();
	TestEqual(TEXT("one warning per bad tile"), W.Num(), 2);
	TestTrue(TEXT("warning names the file and the fix"), W.Num() > 0 && W[0].Contains(TileFile(755, -2450)) && W[0].Contains(TEXT("git lfs pull")));
	Cache.Get(755, -2450);
	Cache.Get(755, -2449);
	TestEqual(TEXT("never retried, never re-warned"), Cache.TakeWarnings().Num(), 0);
	TestEqual(TEXT("no decode counted"), Cache.NumLoads(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverSampleTest, "CamSim.Thermal.LandCover.SanFranciscoSample",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverSampleTest::RunTest(const FString& Parameters)
{
	FLandCoverTileCache Cache(TEXT("Content/NonUFS/LandCover"));
	if (!TestTrue(*FString::Printf(TEXT("committed sample index loads (%s)"), *Cache.GetError()), Cache.IsValid())) return false;
	TestEqual(TEXT("20 tiles cover -122.56 37.69 -122.35 37.84"), Cache.GetIndex().Files.Num(), 20);
	TestTrue(TEXT("CC BY 4.0 attribution"), Cache.GetIndex().Attribution.Contains(TEXT("ESA WorldCover"))
		&& Cache.GetIndex().Licence == TEXT("CC BY 4.0"));
	const auto Presidio = Cache.Get(755, -2450);   // 37.75-37.80 N, 122.50-122.45 W: Presidio, Golden Gate Park, Richmond
	if (!TestTrue(TEXT("Presidio tile decodes (git lfs pull if this fails)"), Presidio.IsValid())) return false;
	int32 Hist[256] = {};
	for (const uint8 C : Presidio->Codes) ++Hist[C];
	TestTrue(TEXT("tree cover present"), Hist[10] > 1000);
	TestTrue(TEXT("built-up present"), Hist[50] > 1000);
	int32 Unknown = 0;
	for (int32 C = 0; C < 256; ++C)
	{
		const bool bWorldCover = C == 0 || C == 95 || (C % 10 == 0 && C <= 100);
		if (!bWorldCover) Unknown += Hist[C];
	}
	TestEqual(TEXT("only WorldCover codes"), Unknown, 0);
	TestEqual(TEXT("no warnings"), Cache.TakeWarnings().Num(), 0);
	return true;
}

// Fix round 1: a tile load (file I/O + PNG decode) runs outside the cache lock, so TakeWarnings / NumCached on the game
// thread return promptly while a build decodes; two threads loading the same tile cache it once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverSlowLoadTest, "CamSim.Thermal.LandCover.SlowLoadDoesNotHoldLock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverSlowLoadTest::RunTest(const FString& Parameters)
{
	const FString Dir = TempDir(TEXT("SlowLoad"));
	WriteTileDir(Dir, { Uniform(755, -2450, 10) });
	FLandCoverTileCache Cache(Dir);
	FLandCoverTileCache::TestLoadDelaySeconds = 0.5;
	ON_SCOPE_EXIT { FLandCoverTileCache::TestLoadDelaySeconds = 0.0; };
	auto Load = [&Cache]() { return Cache.Get(755, -2450); };
	UE::Tasks::TTask<TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe>> A = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Load]() { return Load(); });
	UE::Tasks::TTask<TSharedPtr<const FLandCoverTile, ESPMode::ThreadSafe>> B = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Load]() { return Load(); });
	FPlatformProcess::Sleep(0.1f);
	double WorstMs = 0.0;
	for (int32 K = 0; K < 5; ++K)
	{
		const double T0 = FPlatformTime::Seconds();
		Cache.TakeWarnings();
		Cache.NumCached();
		Cache.NumLoads();
		WorstMs = FMath::Max(WorstMs, (FPlatformTime::Seconds() - T0) * 1000.0);
	}
	AddInfo(FString::Printf(TEXT("cache calls during a 500 ms load: worst %.3f ms"), WorstMs));
	TestFalse(TEXT("the loads are still sleeping"), A.IsCompleted() && B.IsCompleted());
	TestTrue(FString::Printf(TEXT("lock not held across the load (worst %.1f ms)"), WorstMs), WorstMs < 50.0);
	A.Wait();
	B.Wait();
	TestTrue(TEXT("both got the tile"), A.GetResult().IsValid() && B.GetResult().IsValid());
	TestTrue(TEXT("the same cached tile"), A.GetResult() == B.GetResult());
	TestEqual(TEXT("cached once"), Cache.NumCached(), 1);
	TestEqual(TEXT("one load kept"), Cache.NumLoads(), 1);
	return true;
}
