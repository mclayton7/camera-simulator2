// Copyright CamSim Contributors. All Rights Reserved.

#include "Thermal/LandCoverGeometry.h"
#include "Thermal/LandCoverTiles.h"

namespace CamSimLandCover
{
	namespace
	{
		constexpr double A = 6378137.0, E2 = 6.69437999014e-3;   // WGS-84
		constexpr int32  Px = FLandCoverTileCache::TilePx;
		constexpr double Cells = FLandCoverTileCache::CellsPerDeg;
		constexpr int32  TileRowsToEquator = GlobalRows / Px / 2;   // 1800
		constexpr int32  TileColsToMeridian = GlobalCols / Px / 2;  // 3600

		constexpr double CellEps = 1e-6;   // cells: absorbs round-off at exact grid lines (cf. the fetch script round(x, 9))

		double W2(double LatDeg) { const double S = FMath::Sin(FMath::DegreesToRadians(LatDeg)); return 1.0 - E2 * S * S; }
	}

	double MeridionalRadiusM(double LatDeg) { const double W = W2(LatDeg); return A * (1.0 - E2) / (W * FMath::Sqrt(W)); }
	double PrimeVerticalRadiusM(double LatDeg) { return A / FMath::Sqrt(W2(LatDeg)); }

	FVector2D GeodeticToWindowEN(const FWindowSpec& W, double LatDeg, double LonDeg)
	{
		double DLon = FMath::Fmod(LonDeg - W.CentreLonDeg, 360.0);
		if (DLon > 180.0) DLon -= 360.0;
		else if (DLon <= -180.0) DLon += 360.0;
		const double CosPhi = FMath::Cos(FMath::DegreesToRadians(W.CentreLatDeg));
		return FVector2D(FMath::DegreesToRadians(DLon) * PrimeVerticalRadiusM(W.CentreLatDeg) * CosPhi,
			FMath::DegreesToRadians(LatDeg - W.CentreLatDeg) * MeridionalRadiusM(W.CentreLatDeg));
	}

	void WindowENToGeodetic(const FWindowSpec& W, double EastM, double NorthM, double& OutLatDeg, double& OutLonDeg)
	{
		const double CosPhi = FMath::Cos(FMath::DegreesToRadians(W.CentreLatDeg));
		OutLatDeg = W.CentreLatDeg + FMath::RadiansToDegrees(NorthM / MeridionalRadiusM(W.CentreLatDeg));
		OutLonDeg = W.CentreLonDeg + FMath::RadiansToDegrees(EastM / (PrimeVerticalRadiusM(W.CentreLatDeg) * CosPhi));
	}

	FIntPoint GlobalCell(double LatDeg, double LonDeg)
	{
		const int32 Row = FMath::Clamp(FMath::FloorToInt32((90.0 - LatDeg) * Cells + CellEps), 0, GlobalRows - 1);
		int64 Col = FMath::FloorToInt64((LonDeg + 180.0) * Cells + CellEps) % GlobalCols;
		if (Col < 0) Col += GlobalCols;
		return FIntPoint(static_cast<int32>(Col), Row);
	}

	void CellToTile(FIntPoint Cell, FIntPoint& OutTile, FIntPoint& OutInTile)
	{
		OutTile = FIntPoint(TileRowsToEquator - 1 - Cell.Y / Px, Cell.X / Px - TileColsToMeridian);
		OutInTile = FIntPoint(Cell.X % Px, Cell.Y % Px);
	}

	bool IsWindowAllowed(double CentreLatDeg) { return FMath::Abs(CentreLatDeg) <= MaxWindowLatDeg; }

	bool NeedsRecentre(const FWindowSpec& Current, double CamLatDeg, double CamLonDeg, float RecentreFraction)
	{
		const FVector2D EN = GeodeticToWindowEN(Current, CamLatDeg, CamLonDeg);
		const double Limit = static_cast<double>(RecentreFraction) * Current.Texels * Current.TexelM;
		return FMath::Abs(EN.X) > Limit || FMath::Abs(EN.Y) > Limit;
	}

	int64 Resample(const FWindowSpec& W, FTileCodes Tiles, TArray<uint8>& OutCodes)
	{
		const int32 N = W.Texels;
		if (N <= 0)
		{
			OutCodes.Reset();
			return 0;
		}
		OutCodes.SetNumZeroed(N * N);
		if (!IsWindowAllowed(W.CentreLatDeg)) return 0;   // near a pole the row/column math overflows: no data
		TArray<int32> ColCell, RowCell;
		ColCell.SetNumUninitialized(N);
		RowCell.SetNumUninitialized(N);
		for (int32 X = 0; X < N; ++X)
		{
			double Lat = 0.0, Lon = 0.0;
			WindowENToGeodetic(W, (X + 0.5 - 0.5 * N) * W.TexelM, 0.0, Lat, Lon);
			ColCell[X] = GlobalCell(W.CentreLatDeg, Lon).X;
		}
		for (int32 Y = 0; Y < N; ++Y)
		{
			double Lat = 0.0, Lon = 0.0;
			WindowENToGeodetic(W, 0.0, (0.5 * N - Y - 0.5) * W.TexelM, Lat, Lon);
			RowCell[Y] = GlobalCell(Lat, W.CentreLonDeg).Y;
		}
		int64 NonZero = 0;
		FIntPoint CachedTile(MAX_int32, MAX_int32);
		const uint8* Codes = nullptr;
		for (int32 Y = 0; Y < N; ++Y)
		{
			uint8* Dst = OutCodes.GetData() + static_cast<int64>(Y) * N;
			const int32 LatIndex = TileRowsToEquator - 1 - RowCell[Y] / Px;
			const int32 InRow = RowCell[Y] % Px;
			for (int32 X = 0; X < N; ++X)
			{
				const int32 LonIndex = ColCell[X] / Px - TileColsToMeridian;
				if (LatIndex != CachedTile.X || LonIndex != CachedTile.Y)
				{
					CachedTile = FIntPoint(LatIndex, LonIndex);
					Codes = Tiles(LatIndex, LonIndex);
				}
				if (Codes)
				{
					const uint8 C = Codes[InRow * Px + ColCell[X] % Px];
					Dst[X] = C;
					NonZero += C != 0 ? 1 : 0;
				}
			}
		}
		return NonZero;
	}
}
