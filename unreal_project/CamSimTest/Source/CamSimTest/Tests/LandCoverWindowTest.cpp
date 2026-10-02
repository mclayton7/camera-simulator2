// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Thermal/LandCoverWindow.h"
#include "Tests/LandCoverTestTiles.h"

// CamSim.Thermal.LandCover.*: the async, double-buffered window (ROADMAP 4B). GPU uploads are off (NullRHI).

using namespace CamSimLandCoverTest;

namespace
{
	constexpr double CamLat = 37.775, CamLon = -122.45;   // 122.45 W is the boundary between tile columns -2450 and -2449

	/** Rows 753..757 x columns -2452..-2447 (covers a 2048-texel window at the camera): code 10 west of 122.45 W, 50 east. */
	FString WriteSfDir(const TCHAR* Name)
	{
		const FString Dir = TempDir(Name);
		TArray<FTestTile> Tiles;
		for (int32 I = 753; I <= 757; ++I)
			for (int32 J = -2452; J <= -2447; ++J) Tiles.Add(Uniform(I, J, J <= -2450 ? 10 : 50));
		WriteTileDir(Dir, Tiles);
		return Dir;
	}

	FLandCoverWindow::FSettings Settings(const FString& Dir, int32 Texels)
	{
		FLandCoverWindow::FSettings S;
		S.Dir = Dir;
		S.Texels = Texels;
		S.RecentreFraction = 0.25f;
		S.bCreateGpuTexture = false;
		return S;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverWindowSwapTest, "CamSim.Thermal.LandCover.WindowBuildsAsyncAndSwaps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverWindowSwapTest::RunTest(const FString& Parameters)
{
	FLandCoverWindow W;
	W.Configure(Settings(WriteSfDir(TEXT("Swap")), 256));   // 2.56 km: re-centre beyond 640 m
	TestTrue(TEXT("index loaded"), W.IsAvailable());
	W.Update(CamLat, CamLon);
	TestTrue(TEXT("first build started"), W.IsBuildInFlight());
	TestFalse(TEXT("no window until it finishes"), W.GetCurrent().IsValid());
	W.FinishBuildForTest();
	const auto First = W.GetCurrent();
	if (!TestTrue(TEXT("first window published"), First.IsValid())) return false;
	TestEqual(TEXT("id 1"), First->Id, 1u);
	TestEqual(TEXT("size"), First->Codes.Num(), 256 * 256);
	TestEqual(TEXT("west of the boundary: tile code 10"), First->Codes[128 * 256 + 10], static_cast<uint8>(10));
	TestEqual(TEXT("east of the boundary: tile code 50"), First->Codes[128 * 256 + 245], static_cast<uint8>(50));
	TestTrue(TEXT("GPU handle paired"), W.GetCurrentGpu().IsValid() && W.GetCurrentGpu()->Id == 1u && W.GetCurrentGpu()->Texels == 256);
	W.Update(CamLat, CamLon);
	TestEqual(TEXT("same pose: no rebuild"), W.GetBuildsStarted(), 1u);
	double Lat = 0.0, Lon = 0.0;
	CamSimLandCover::WindowENToGeodetic(First->Spec, 0.0, 768.0, Lat, Lon);
	W.Update(Lat, Lon);
	TestTrue(TEXT("768 m north: rebuild started"), W.IsBuildInFlight() || W.GetBuildsStarted() == 2u);
	TestEqual(TEXT("the old window keeps rendering meanwhile"), W.GetCurrent()->Id, 1u);
	W.FinishBuildForTest();
	TestEqual(TEXT("swapped to id 2"), W.GetCurrent()->Id, 2u);
	TestNearlyEqual(TEXT("re-centred on the camera"), W.GetCurrent()->Spec.CentreLatDeg, Lat, 1e-12);
	W.Update(Lat, Lon);
	W.Update(Lat, Lon);
	TestEqual(TEXT("one build per re-centre"), W.GetBuildsStarted(), 2u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverWindowUnavailableTest, "CamSim.Thermal.LandCover.MissingDirAndPole",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverWindowUnavailableTest::RunTest(const FString& Parameters)
{
	FLandCoverWindow Missing;
	Missing.Configure(Settings(TempDir(TEXT("Empty")) / TEXT("nope"), 256));
	Missing.Update(CamLat, CamLon);
	Missing.Update(CamLat, CamLon);
	TestFalse(TEXT("unavailable"), Missing.IsAvailable());
	TestEqual(TEXT("no build"), Missing.GetBuildsStarted(), 0u);
	TestEqual(TEXT("one warning"), Missing.TakeWarnings().Num(), 1);
	Missing.Update(CamLat, CamLon);
	TestEqual(TEXT("not repeated"), Missing.TakeWarnings().Num(), 0);

	FLandCoverWindow Pole;
	Pole.Configure(Settings(WriteSfDir(TEXT("Pole")), 256));
	Pole.Update(89.5, 0.0);
	Pole.Update(89.6, 0.0);
	TestEqual(TEXT("no window within 1 deg of a pole"), Pole.GetBuildsStarted(), 0u);
	TestEqual(TEXT("one pole warning"), Pole.TakeWarnings().Num(), 1);
	TestFalse(TEXT("no window"), Pole.GetCurrent().IsValid());
	return true;
}

// Review Focus 2: out of the data's coverage -> land cover off with one warning, no rebuild per frame, back when over data.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverWindowNoDataTest, "CamSim.Thermal.LandCover.WindowWithoutDataIsOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverWindowNoDataTest::RunTest(const FString& Parameters)
{
	FLandCoverWindow W;
	W.Configure(Settings(WriteSfDir(TEXT("NoData")), 256));
	W.Update(0.025, 0.025);
	W.FinishBuildForTest();
	TestFalse(TEXT("all-zero window is not published"), W.GetCurrent().IsValid());
	TestFalse(TEXT("no GPU window either"), W.GetCurrentGpu().IsValid());
	const TArray<FString> Warn = W.TakeWarnings();
	TestTrue(TEXT("one no-data warning"), Warn.Num() == 1 && Warn[0].Contains(TEXT("no land-cover data")));
	W.Update(0.025, 0.025);
	W.Update(0.025, 0.025);
	TestEqual(TEXT("no rebuild per frame"), W.GetBuildsStarted(), 1u);
	W.Update(CamLat, CamLon);
	W.FinishBuildForTest();
	TestTrue(TEXT("back over data: published again"), W.GetCurrent().IsValid() && W.GetCurrent()->NonZeroTexels > 0);
	return true;
}

// Review Focus 3: a reconfigure during a build never publishes the stale result.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverWindowReconfigureTest, "CamSim.Thermal.LandCover.ReconfigureDiscardsInFlightBuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverWindowReconfigureTest::RunTest(const FString& Parameters)
{
	const FString Dir = WriteSfDir(TEXT("Reconfigure"));
	FLandCoverWindow W;
	W.Configure(Settings(Dir, 256));
	W.Update(CamLat, CamLon);
	TestTrue(TEXT("building"), W.IsBuildInFlight());
	W.Configure(Settings(Dir, 128));
	W.FinishBuildForTest();
	TestFalse(TEXT("the 256 build is discarded"), W.GetCurrent().IsValid());
	W.Update(CamLat, CamLon);
	W.FinishBuildForTest();
	if (!TestTrue(TEXT("new build published"), W.GetCurrent().IsValid())) return false;
	TestEqual(TEXT("with the new size"), W.GetCurrent()->Spec.Texels, 128);
	TestEqual(TEXT("codes match"), W.GetCurrent()->Codes.Num(), 128 * 128);
	W.Configure(Settings(Dir, 128));
	TestTrue(TEXT("identical settings keep the window"), W.GetCurrent().IsValid());
	return true;
}

// Review Focus 1 at window level: a bad tile becomes zeros, warned once by path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverWindowCorruptTest, "CamSim.Thermal.LandCover.CorruptTileInWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverWindowCorruptTest::RunTest(const FString& Parameters)
{
	const FString Dir = WriteSfDir(TEXT("CorruptWindow"));
	FFileHelper::SaveArrayToFile(LfsPointerBytes(), *FPaths::Combine(Dir, TileFile(755, -2450)));
	FLandCoverWindow W;
	W.Configure(Settings(Dir, 256));
	W.Update(CamLat, CamLon);
	W.FinishBuildForTest();
	if (!TestTrue(TEXT("published (east half has data)"), W.GetCurrent().IsValid())) return false;
	TestEqual(TEXT("bad tile -> 0"), W.GetCurrent()->Codes[128 * 256 + 10], static_cast<uint8>(0));
	TestEqual(TEXT("good tile kept"), W.GetCurrent()->Codes[128 * 256 + 245], static_cast<uint8>(50));
	const TArray<FString> Warn = W.TakeWarnings();
	TestEqual(TEXT("one warning for the tile"), Warn.FilterByPredicate([](const FString& S) { return S.Contains(TileFile(755, -2450)); }).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverWindowBuildTimeTest, "CamSim.Thermal.LandCover.WindowBuildTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverWindowBuildTimeTest::RunTest(const FString& Parameters)
{
	FLandCoverWindow W;
	W.Configure(Settings(WriteSfDir(TEXT("BuildTime")), 2048));
	W.Update(CamLat, CamLon);
	W.FinishBuildForTest();
	if (!TestTrue(TEXT("published"), W.GetCurrent().IsValid())) return false;
	const double Ms = W.GetCurrent()->BuildMs;
	AddInfo(FString::Printf(TEXT("2048^2 window: %.1f ms (%d tiles; target < 100 ms)"), Ms, W.GetCurrent()->TilesUsed));
	if (Ms > 100.0) AddWarning(FString::Printf(TEXT("window build over the 100 ms target: %.1f ms"), Ms));
	TestTrue(TEXT("well under a second"), Ms < 1000.0);
	TestEqual(TEXT("every texel covered"), W.GetCurrent()->NonZeroTexels, static_cast<int64>(2048) * 2048);
	TestEqual(TEXT("no tile missing"), W.GetCurrent()->TilesMissing, 0);
	return true;
}

// Carry-over (a): a window touching more tiles than the cache holds pins every tile for the whole resample, so codes come
// from live tiles (no dangling pointer after LRU eviction) and each tile is decoded once (no thrash).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverWindowPinTest, "CamSim.Thermal.LandCover.WindowNeedsMoreTilesThanCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverWindowPinTest::RunTest(const FString& Parameters)
{
	const FString Dir = TempDir(TEXT("Pin"));
	TArray<FTestTile> Tiles;
	TMap<FIntPoint, int32> Slot;
	for (int32 I = 753; I <= 757; ++I)
		for (int32 J = -2452; J <= -2447; ++J)
		{
			Slot.Add(FIntPoint(I, J), Tiles.Num());
			Tiles.Add(Uniform(I, J, static_cast<uint8>(1 + Tiles.Num())));   // a distinct code per tile
		}
	WriteTileDir(Dir, Tiles);
	FLandCoverWindow::FSettings S = Settings(Dir, 2048);
	S.MaxCachedTiles = 4;   // the cache's minimum; the window touches ~20 tiles
	FLandCoverWindow W;
	W.Configure(S);
	W.Update(CamLat, CamLon);
	W.FinishBuildForTest();
	if (!TestTrue(TEXT("published"), W.GetCurrent().IsValid())) return false;
	TestTrue(TEXT("more tiles than the cache holds"), W.GetCurrent()->TilesUsed > S.MaxCachedTiles);
	TArray<uint8> Expected;
	const int64 NonZero = CamSimLandCover::Resample(W.GetCurrent()->Spec, [&Tiles, &Slot](int32 I, int32 J) -> const uint8*
	{
		const int32* K = Slot.Find(FIntPoint(I, J));
		return K ? Tiles[*K].Codes.GetData() : nullptr;
	}, Expected);
	TestEqual(TEXT("every texel covered"), W.GetCurrent()->NonZeroTexels, NonZero);
	TestTrue(TEXT("codes identical to an uncached resample"), W.GetCurrent()->Codes == Expected);
	TestEqual(TEXT("no warnings"), W.TakeWarnings().Num(), 0);
	return true;
}

// Fix round 1: Update / TakeWarnings never wait for a build that is decoding tiles (no hitch on re-centre).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverWindowNoBlockTest, "CamSim.Thermal.LandCover.UpdateNeverWaitsForBuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverWindowNoBlockTest::RunTest(const FString& Parameters)
{
	FLandCoverWindow W;
	W.Configure(Settings(WriteSfDir(TEXT("NoBlock")), 256));   // 2 tiles at the camera
	FLandCoverTileCache::TestLoadDelaySeconds = 0.4;
	ON_SCOPE_EXIT { FLandCoverTileCache::TestLoadDelaySeconds = 0.0; };
	W.Update(CamLat, CamLon);
	FPlatformProcess::Sleep(0.05f);   // the build is now inside its first tile load
	double WorstMs = 0.0;
	for (int32 K = 0; K < 10; ++K)
	{
		const double T0 = FPlatformTime::Seconds();
		W.Update(CamLat, CamLon);
		W.TakeWarnings();
		WorstMs = FMath::Max(WorstMs, (FPlatformTime::Seconds() - T0) * 1000.0);
	}
	AddInfo(FString::Printf(TEXT("Update + TakeWarnings during a slow build: worst %.3f ms"), WorstMs));
	TestTrue(TEXT("still building"), W.IsBuildInFlight());
	TestTrue(FString::Printf(TEXT("Update + TakeWarnings return promptly (worst %.1f ms)"), WorstMs), WorstMs < 50.0);
	FLandCoverTileCache::TestLoadDelaySeconds = 0.0;
	W.FinishBuildForTest();
	TestTrue(TEXT("published"), W.GetCurrent().IsValid());
	return true;
}

// Fix round 1: flying over a pole after a window exists turns land cover off, and it rebuilds on the way back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverWindowPoleAfterTest, "CamSim.Thermal.LandCover.PoleAfterPublishedWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverWindowPoleAfterTest::RunTest(const FString& Parameters)
{
	FLandCoverWindow W;
	W.Configure(Settings(WriteSfDir(TEXT("PoleAfter")), 256));
	W.Update(CamLat, CamLon);
	W.FinishBuildForTest();
	if (!TestTrue(TEXT("window published"), W.GetCurrent().IsValid() && W.GetCurrentGpu().IsValid())) return false;
	W.TakeWarnings();
	W.Update(89.5, 0.0);
	TestFalse(TEXT("pole: no CPU window"), W.GetCurrent().IsValid());
	TestFalse(TEXT("pole: no GPU window"), W.GetCurrentGpu().IsValid());
	TestEqual(TEXT("pole: no build"), W.GetBuildsStarted(), 1u);
	TestEqual(TEXT("one pole warning"), W.TakeWarnings().Num(), 1);
	W.Update(CamLat, CamLon);
	TestEqual(TEXT("back: rebuild started"), W.GetBuildsStarted(), 2u);
	W.FinishBuildForTest();
	TestTrue(TEXT("back: window id 2"), W.GetCurrent().IsValid() && W.GetCurrent()->Id == 2u && W.GetCurrentGpu()->Id == 2u);
	return true;
}

// Fix round 1: the pin set's size estimate covers every tile a window touches (mid latitude, antimeridian, 88.9 deg).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverWindowEstimateTest, "CamSim.Thermal.LandCover.PinSetEstimateCoversWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverWindowEstimateTest::RunTest(const FString& Parameters)
{
	const FVector2D Centres[] = { { CamLat, CamLon }, { 0.0, 179.99 }, { 0.0, -179.99 }, { 88.9, 0.0 }, { -88.95, 179.95 } };
	for (const FVector2D& C : Centres)
	{
		CamSimLandCover::FWindowSpec S;
		S.CentreLatDeg = C.X; S.CentreLonDeg = C.Y; S.Texels = 2048; S.TexelM = FLandCoverWindow::TexelM;
		TSet<FIntPoint> Touched;
		TArray<uint8> Out;
		CamSimLandCover::Resample(S, [&Touched](int32 I, int32 J) -> const uint8* { Touched.Add(FIntPoint(I, J)); return nullptr; }, Out);
		const int32 Estimate = FLandCoverWindow::EstimateTiles(S);
		TestTrue(FString::Printf(TEXT("(%.2f, %.2f): estimate %d >= %d tiles touched"), C.X, C.Y, Estimate, Touched.Num()),
			Estimate >= Touched.Num());
		TestTrue(FString::Printf(TEXT("(%.2f, %.2f): estimate %d not wildly over %d"), C.X, C.Y, Estimate, Touched.Num()),
			Estimate <= 2 * Touched.Num() + 16);
	}
	return true;
}

// Review Focus 4: a frame binds a window texture only when it is the window its parameters were built against.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandCoverPairingTest, "CamSim.Thermal.LandCover.FramePairingById",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLandCoverPairingTest::RunTest(const FString& Parameters)
{
	using CamSimLandCover::ShouldBindWindow;
	TestTrue (TEXT("same window, texture uploaded"), ShouldBindWindow(1u, 7u, 7u, true));
	TestFalse(TEXT("params from window 7, texture from window 8 (swap mid-flight)"), ShouldBindWindow(1u, 7u, 8u, true));
	TestFalse(TEXT("upload not run yet"), ShouldBindWindow(1u, 7u, 7u, false));
	TestFalse(TEXT("land cover off in the params"), ShouldBindWindow(0u, 7u, 7u, true));
	TestFalse(TEXT("no window id"), ShouldBindWindow(1u, 0u, 0u, true));

	// Every re-centre publishes a new id, so params built before a swap can never match the window published after it.
	FLandCoverWindow W;
	W.Configure(Settings(WriteSfDir(TEXT("Pairing")), 256));
	W.Update(CamLat, CamLon);
	W.FinishBuildForTest();
	if (!TestTrue(TEXT("first window"), W.GetCurrentGpu().IsValid())) return false;
	const uint32 First = W.GetCurrentGpu()->Id;
	double Lat = 0.0, Lon = 0.0;
	CamSimLandCover::WindowENToGeodetic(W.GetCurrent()->Spec, 0.0, 800.0, Lat, Lon);
	W.Update(Lat, Lon);
	W.FinishBuildForTest();
	const uint32 Second = W.GetCurrentGpu()->Id;
	TestNotEqual(TEXT("new id per window"), First, Second);
	TestEqual(TEXT("CPU and GPU halves share the id"), W.GetCurrent()->Id, Second);
	TestFalse(TEXT("old params never bind the new texture"), ShouldBindWindow(1u, First, Second, true));
	return true;
}
