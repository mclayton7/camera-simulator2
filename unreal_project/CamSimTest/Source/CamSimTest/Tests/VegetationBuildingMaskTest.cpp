// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Async/ParallelFor.h"
#include "IImageWrapperModule.h"
#include "Misc/AutomationTest.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "Tests/LandCoverTestTiles.h"
#include "Thermal/LandCoverGeometry.h"
#include "Vegetation/VegetationBuildingMask.h"

namespace CamSimVegMaskTest
{
	constexpr int32 Px = FVegetationBuildingMask::TilePx;
	constexpr double Half = 0.5 / FVegetationBuildingMask::CellsPerDeg;   // half a mask cell, degrees

	uint32 PngCrc(const uint8* D, int64 N)
	{
		uint32 C = 0xFFFFFFFFu;
		for (int64 I = 0; I < N; ++I)
		{
			C ^= D[I];
			for (int32 K = 0; K < 8; ++K) C = (C >> 1) ^ (0xEDB88320u & (0u - (C & 1u)));
		}
		return ~C;
	}

	void PutBE32(TArray<uint8>& O, uint32 V) { O.Add(uint8(V >> 24)); O.Add(uint8(V >> 16)); O.Add(uint8(V >> 8)); O.Add(uint8(V)); }

	void PutChunk(TArray<uint8>& O, const char* Type, const TArray<uint8>& Data)
	{
		PutBE32(O, static_cast<uint32>(Data.Num()));
		const int32 Start = O.Num();
		for (int32 K = 0; K < 4; ++K) O.Add(uint8(Type[K]));
		O.Append(Data);
		PutBE32(O, PngCrc(O.GetData() + Start, O.Num() - Start));
	}

	/** A Px x Px 1-bit greyscale PNG, as camsim-scene writes the mask (Pillow mode "1"): Set(column, row), row 0 = north. */
	TArray<uint8> MakeOneBitPng(TFunctionRef<bool(int32, int32)> Set)
	{
		const int32 RowBytes = Px / 8;
		TArray<uint8> Raw;
		Raw.SetNumZeroed((RowBytes + 1) * Px);   // filter byte 0 (none) + packed row, MSB first
		for (int32 Y = 0; Y < Px; ++Y)
		{
			uint8* R = Raw.GetData() + Y * (RowBytes + 1) + 1;
			for (int32 X = 0; X < Px; ++X) if (Set(X, Y)) R[X >> 3] |= uint8(0x80u >> (X & 7));
		}
		int32 ZLen = FCompression::CompressMemoryBound(NAME_Zlib, Raw.Num());
		TArray<uint8> Z;
		Z.SetNumUninitialized(ZLen);
		verify(FCompression::CompressMemory(NAME_Zlib, Z.GetData(), ZLen, Raw.GetData(), Raw.Num()));
		Z.SetNum(ZLen);
		TArray<uint8> Ihdr;
		PutBE32(Ihdr, Px); PutBE32(Ihdr, Px);
		Ihdr.Append({ 1, 0, 0, 0, 0 });   // bit depth 1, greyscale, deflate, filter 0, no interlace
		TArray<uint8> Png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		PutChunk(Png, "IHDR", Ihdr);
		PutChunk(Png, "IDAT", Z);
		PutChunk(Png, "IEND", TArray<uint8>());
		return Png;
	}

	FString TileFile(FIntPoint Tile) { return FString::Printf(TEXT("%+.2f_%+.2f.png"), Tile.X * 0.05, Tile.Y * 0.05); }

	/** A mask directory with these tiles (file bytes as given) in its index. */
	FString WriteMaskDir(const TCHAR* Name, const TArray<TPair<FIntPoint, TArray<uint8>>>& Tiles, const TArray<FIntPoint>& IndexedOnly = {})
	{
		const FString Dir = CamSimLandCoverTest::TempDir(Name);
		FString Entries;
		auto Entry = [&Entries](FIntPoint K)
		{
			Entries += FString::Printf(TEXT("%s{\"file\": \"%s\", \"lat_index\": %d, \"lon_index\": %d}"),
				Entries.IsEmpty() ? TEXT("") : TEXT(", "), *TileFile(K), K.X, K.Y);
		};
		for (const auto& T : Tiles) { FFileHelper::SaveArrayToFile(T.Value, *(Dir / TileFile(T.Key))); Entry(T.Key); }
		for (const FIntPoint K : IndexedOnly) Entry(K);
		FFileHelper::SaveStringToFile(FString::Printf(
			TEXT("{\"format\": \"camsim-buildings-mask-1\", \"tile_deg\": 0.05, \"tile_px\": 4800, \"cell_factor\": 8, ")
			TEXT("\"rules\": \"1\", \"tiles\": [%s]}"), *Entries), *(Dir / TEXT("index.json")));
		return Dir;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegMaskCellOfTest, "CamSim.Vegetation.BuildingMask.CellOfMatchesTheBuildGrid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegMaskCellOfTest::RunTest(const FString& Parameters)
{
	using namespace CamSimVegMaskTest;
	FIntPoint Tile, In;
	FVegetationBuildingMask::CellOf(33.3001, -117.4999, Tile, In);   // camsim-scene: tile (666, -2350), column 9, row 4790
	TestEqual(TEXT("tile"), Tile, FIntPoint(666, -2350));
	TestEqual(TEXT("cell"), In, FIntPoint(9, 4790));
	FVegetationBuildingMask::CellOf(33.30 + Half, -117.30 + Half, Tile, In);   // just NE of a tile corner
	TestEqual(TEXT("NE tile"), Tile, FIntPoint(666, -2346));
	TestEqual(TEXT("its SW cell"), In, FIntPoint(0, Px - 1));
	FVegetationBuildingMask::CellOf(33.30 - Half, -117.30 - Half, Tile, In);
	TestEqual(TEXT("SW tile"), Tile, FIntPoint(665, -2347));
	TestEqual(TEXT("its NE cell"), In, FIntPoint(Px - 1, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegMaskDecodeTest, "CamSim.Vegetation.BuildingMask.DecodesOneAndEightBitPngs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegMaskDecodeTest::RunTest(const FString& Parameters)
{
	using namespace CamSimVegMaskTest;
	IImageWrapperModule& M = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	auto Bit = [](const TArray<uint64>& B, int32 X, int32 Y) { const int64 K = int64(Y) * Px + X; return ((B[K >> 6] >> (K & 63)) & 1u) != 0; };
	TArray<uint64> Bits;
	FString Error;
	const TArray<uint8> One = MakeOneBitPng([](int32 X, int32 Y) { return (X == 0 && Y == 0) || (X == Px - 1 && Y == 0) || (X == 7 && Y == 9); });
	if (TestTrue(*FString::Printf(TEXT("1-bit decodes (%s)"), *Error), FVegetationBuildingMask::DecodeMaskPng(M, One, Bits, Error)))
	{
		TestEqual(TEXT("words"), Bits.Num(), FVegetationBuildingMask::WordsPerTile);
		TestTrue(TEXT("(0, 0)"), Bit(Bits, 0, 0));
		TestTrue(TEXT("(4799, 0)"), Bit(Bits, Px - 1, 0));
		TestTrue(TEXT("(7, 9)"), Bit(Bits, 7, 9));
		TestFalse(TEXT("(1, 0)"), Bit(Bits, 1, 0));
		int64 Set = 0;
		for (const uint64 W : Bits) Set += FMath::CountBits64(W);
		TestEqual(TEXT("exactly three set"), Set, int64(3));
	}
	TArray<uint8> Grey;
	Grey.SetNumZeroed(Px * Px);
	Grey[12 * Px + 34] = 255;
	const TArray64<uint8> Eight = CamSimLandCoverTest::EncodeGreyPng(Grey, Px, Px);
	if (TestTrue(TEXT("8-bit decodes"), FVegetationBuildingMask::DecodeMaskPng(M, TConstArrayView<uint8>(Eight.GetData(), static_cast<int32>(Eight.Num())), Bits, Error)))
	{
		TestTrue(TEXT("(34, 12)"), Bit(Bits, 34, 12));
	}
	TArray<uint8> Small;
	Small.SetNumZeroed(600 * 600);
	const TArray64<uint8> Wrong = CamSimLandCoverTest::EncodeGreyPng(Small, 600, 600);
	TestFalse(TEXT("a land-cover-sized PNG is refused"), FVegetationBuildingMask::DecodeMaskPng(M, TConstArrayView<uint8>(Wrong.GetData(), static_cast<int32>(Wrong.Num())), Bits, Error));
	TestFalse(TEXT("an LFS pointer is refused"), FVegetationBuildingMask::DecodeMaskPng(M, CamSimLandCoverTest::LfsPointerBytes(), Bits, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegMaskCornersTest, "CamSim.Vegetation.BuildingMask.TileEdgesAndCorners",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegMaskCornersTest::RunTest(const FString& Parameters)
{
	using namespace CamSimVegMaskTest;
	// Four tiles meet at (33.30, -117.30); each sets only its cell at that corner.
	const FIntPoint NE(666, -2346), NW(666, -2347), SE(665, -2346), SW(665, -2347);
	const FString Dir = WriteMaskDir(TEXT("MaskCorners"), {
		{ NE, MakeOneBitPng([](int32 X, int32 Y) { return X == 0 && Y == Px - 1; }) },
		{ NW, MakeOneBitPng([](int32 X, int32 Y) { return X == Px - 1 && Y == Px - 1; }) },
		{ SE, MakeOneBitPng([](int32 X, int32 Y) { return X == 0 && Y == 0; }) },
		{ SW, MakeOneBitPng([](int32 X, int32 Y) { return X == Px - 1 && Y == 0; }) } });
	FVegetationBuildingMask Mask(Dir);
	if (!TestTrue(*FString::Printf(TEXT("valid (%s)"), *Mask.GetError()), Mask.IsValid())) return false;
	TestTrue(TEXT("NE corner cell"), Mask.IsMasked(33.30 + Half, -117.30 + Half));
	TestTrue(TEXT("NW corner cell"), Mask.IsMasked(33.30 + Half, -117.30 - Half));
	TestTrue(TEXT("SE corner cell"), Mask.IsMasked(33.30 - Half, -117.30 + Half));
	TestTrue(TEXT("SW corner cell"), Mask.IsMasked(33.30 - Half, -117.30 - Half));
	TestFalse(TEXT("one cell north of NE"), Mask.IsMasked(33.30 + 3.0 * Half, -117.30 + Half));
	TestFalse(TEXT("one cell east of SE"), Mask.IsMasked(33.30 - Half, -117.30 + 3.0 * Half));
	TestEqual(TEXT("four tiles loaded"), Mask.NumLoads(), 4);
	// A tree 1.5 cells east of the corner, in the NE tile's bottom row: its west probe (2 m, ~2.1 cells) lands in NW's corner cell
	const double DLon = 1.5 * 2.0 * Half;
	TestFalse(TEXT("west probe hits the NW corner cell"), Mask.IsClear(33.30 + Half, -117.30 + DLon, 2.0));
	TestTrue(TEXT("clearance 0 tests the trunk only"), Mask.IsClear(33.30 + Half, -117.30 + DLon, 0.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegMaskBadTilesTest, "CamSim.Vegetation.BuildingMask.BadTilesAreClear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegMaskBadTilesTest::RunTest(const FString& Parameters)
{
	using namespace CamSimVegMaskTest;
	const FIntPoint Good(666, -2346), Lfs(666, -2347), Gone(665, -2346);
	const FString Dir = WriteMaskDir(TEXT("MaskBad"), {
		{ Good, MakeOneBitPng([](int32, int32) { return true; }) },
		{ Lfs, CamSimLandCoverTest::LfsPointerBytes() } }, { Gone });   // Gone: in the index, no file
	FVegetationBuildingMask Mask(Dir);
	if (!TestTrue(TEXT("valid"), Mask.IsValid())) return false;
	// One warning per bad tile, however often it is asked for.
	AddExpectedMessagePlain(TileFile(Lfs), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TileFile(Gone), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TestTrue(TEXT("good tile answers"), Mask.IsMasked(33.31, -117.29));
	TestFalse(TEXT("LFS pointer tile = no buildings"), Mask.IsMasked(33.31, -117.31));
	TestFalse(TEXT("missing file = no buildings"), Mask.IsMasked(33.29, -117.29));
	TestFalse(TEXT("tile not in the index = no buildings"), Mask.IsMasked(10.0, 10.0));
	TestFalse(TEXT("asked again: still clear, no reload"), Mask.IsMasked(33.31, -117.31));
	TestFalse(TEXT("missing file asked again"), Mask.IsMasked(33.29, -117.29));
	TestTrue(TEXT("good tile still answers"), Mask.IsMasked(33.32, -117.28));
	TestEqual(TEXT("only the good tile decoded"), Mask.NumLoads(), 1);
	FVegetationBuildingMask NoIndex(CamSimLandCoverTest::TempDir(TEXT("MaskNone")));
	TestFalse(TEXT("no index.json: invalid"), NoIndex.IsValid());
	TestFalse(TEXT("invalid: clear everywhere"), NoIndex.IsMasked(33.31, -117.29));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegMaskFivePointTest, "CamSim.Vegetation.BuildingMask.FivePointRule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegMaskFivePointTest::RunTest(const FString& Parameters)
{
	using namespace CamSimVegMaskTest;
	// A building east of Lon0 and north of Lat0 (its south-west corner at (Lat0, Lon0)); metres -> degrees at Lat0.
	const double Lat0 = 33.3, Lon0 = -117.3;
	const double MLat = FMath::DegreesToRadians(1.0) * CamSimLandCover::MeridionalRadiusM(Lat0);
	const double MLon = FMath::DegreesToRadians(1.0) * CamSimLandCover::PrimeVerticalRadiusM(Lat0) * FMath::Cos(FMath::DegreesToRadians(Lat0));
	auto In = [&](double La, double Lo) { return La >= Lat0 && Lo >= Lon0; };
	auto At = [&](double NorthM, double EastM, double Clearance)
	{
		return FVegetationBuildingMask::IsClearOf(Lat0 + NorthM / MLat, Lon0 + EastM / MLon, Clearance, In);
	};
	TestFalse(TEXT("trunk inside"), At(5.0, 5.0, 2.0));
	TestFalse(TEXT("1.5 m west of the wall: east probe inside"), At(5.0, -1.5, 2.0));
	TestTrue (TEXT("2.5 m west of the wall: clear"), At(5.0, -2.5, 2.0));
	TestFalse(TEXT("1.5 m south of the wall: north probe inside"), At(-1.5, 5.0, 2.0));
	TestTrue (TEXT("clearance 0: only the trunk"), At(5.0, -1.5, 0.0));
	TestTrue (TEXT("diagonal from the corner: axis probes miss it (documented limit)"), At(-1.0, -1.0, 2.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegMaskThreadsTest, "CamSim.Vegetation.BuildingMask.AnyThreadAndLru",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVegMaskThreadsTest::RunTest(const FString& Parameters)
{
	using namespace CamSimVegMaskTest;
	// Ten tiles in a row along 33.30 N; tile k is all set when k is odd, empty when even.
	TArray<TPair<FIntPoint, TArray<uint8>>> Tiles;
	for (int32 K = 0; K < 10; ++K)
		Tiles.Add({ FIntPoint(666, -2350 + K), MakeOneBitPng([K](int32, int32) { return (K & 1) != 0; }) });
	FVegetationBuildingMask Mask(WriteMaskDir(TEXT("MaskThreads"), Tiles));
	if (!TestTrue(TEXT("valid"), Mask.IsValid())) return false;
	TArray<int32> Wrong;
	Wrong.SetNumZeroed(400);
	ParallelFor(400, [&Mask, &Wrong](int32 I)
	{
		const int32 K = (I * 7) % 10;
		const double Lon = (-2350 + K) * 0.05 + 0.025;
		Wrong[I] = Mask.IsMasked(33.325, Lon) != ((K & 1) != 0) ? 1 : 0;
	});
	int32 Bad = 0;
	for (const int32 W : Wrong) Bad += W;
	TestEqual(TEXT("every answer right"), Bad, 0);
	TestTrue(*FString::Printf(TEXT("at most %d tiles cached (%d)"), FVegetationBuildingMask::MaxTiles, Mask.NumCached()),
		Mask.NumCached() <= FVegetationBuildingMask::MaxTiles);
	return true;
}
