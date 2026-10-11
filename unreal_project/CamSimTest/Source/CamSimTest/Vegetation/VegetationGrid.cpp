// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationGrid.h"

namespace CamSimVegetation
{
	namespace
	{
		double WrapLon360(double LonDeg)   // [0, 360) measured from 180 W
		{
			double X = FMath::Fmod(LonDeg + 180.0, 360.0);
			return X < 0.0 ? X + 360.0 : X;
		}
		double RowCentreLatDeg(int32 Row, double CellM) { return -90.0 + (Row + 0.5) * CellDLatDeg(CellM); }
	}

	bool IsLatAllowed(double LatDeg) { return FMath::IsFinite(LatDeg) && FMath::Abs(LatDeg) <= MaxLatDeg; }

	double CellDLatDeg(double CellM) { return CellM / MetresPerDegLat; }

	double CellDLonDeg(int32 Row, double CellM)
	{
		const double C = FMath::Max(FMath::Cos(FMath::DegreesToRadians(RowCentreLatDeg(Row, CellM))), 0.01);
		return CellM / (MetresPerDegLat * C);
	}

	int32 NumCols(int32 Row, double CellM) { return FMath::CeilToInt32(360.0 / CellDLonDeg(Row, CellM)); }

	FVegCellId CellOf(double LatDeg, double LonDeg, double CellM)
	{
		FVegCellId Id;
		Id.Row = FMath::FloorToInt32((LatDeg + 90.0) / CellDLatDeg(CellM));
		Id.Col = FMath::Min(FMath::FloorToInt32(WrapLon360(LonDeg) / CellDLonDeg(Id.Row, CellM)), NumCols(Id.Row, CellM) - 1);
		return Id;
	}

	FVegCellBounds CellBounds(FVegCellId Id, double CellM)
	{
		const double DLat = CellDLatDeg(CellM), DLon = CellDLonDeg(Id.Row, CellM);
		FVegCellBounds B;
		B.SouthDeg = -90.0 + Id.Row * DLat;
		B.NorthDeg = B.SouthDeg + DLat;
		B.WestDeg = -180.0 + Id.Col * DLon;
		B.EastDeg = FMath::Min(B.WestDeg + DLon, 180.0);
		return B;
	}

	void CellCentre(FVegCellId Id, double CellM, double& OutLatDeg, double& OutLonDeg)
	{
		const FVegCellBounds B = CellBounds(Id, CellM);
		OutLatDeg = 0.5 * (B.SouthDeg + B.NorthDeg);
		OutLonDeg = 0.5 * (B.WestDeg + B.EastDeg);
	}

	double GroundDistanceM(double Lat1, double Lon1, double Lat2, double Lon2)
	{
		double DLon = FMath::Fmod(Lon2 - Lon1, 360.0);
		if (DLon > 180.0) DLon -= 360.0;
		if (DLon <= -180.0) DLon += 360.0;
		const double N = (Lat2 - Lat1) * MetresPerDegLat;
		const double E = DLon * MetresPerDegLat * FMath::Cos(FMath::DegreesToRadians(0.5 * (Lat1 + Lat2)));
		return FMath::Sqrt(N * N + E * E);
	}

	TArray<FVegCellId> CellsWithin(double LatDeg, double LonDeg, double RadiusM, double CellM)
	{
		TArray<FVegCellId> Out;
		if (!IsLatAllowed(LatDeg) || !(RadiusM > 0.0) || !(CellM > 0.0)) return Out;
		const double DLatR = RadiusM / MetresPerDegLat;
		const int32 Row0 = CellOf(LatDeg - DLatR, LonDeg, CellM).Row, Row1 = CellOf(LatDeg + DLatR, LonDeg, CellM).Row;
		TSet<FVegCellId> Seen;
		TArray<TPair<double, FVegCellId>> Hits;
		for (int32 Row = Row0; Row <= Row1; ++Row)
		{
			const double RowLat = RowCentreLatDeg(Row, CellM);
			if (!IsLatAllowed(RowLat)) continue;
			const double DLon = CellDLonDeg(Row, CellM);
			const int32 Cols = NumCols(Row, CellM);
			const double SpanDeg = RadiusM / (MetresPerDegLat * FMath::Max(FMath::Cos(FMath::DegreesToRadians(RowLat)), 0.01)) + DLon;
			const int32 C0 = FMath::FloorToInt32(WrapLon360(LonDeg - SpanDeg) / DLon);
			const int32 Count = FMath::Min(FMath::CeilToInt32(2.0 * SpanDeg / DLon) + 2, Cols);
			for (int32 K = 0; K < Count; ++K)
			{
				const FVegCellId Id{ Row, ((C0 + K) % Cols + Cols) % Cols };
				if (Seen.Contains(Id)) continue;
				Seen.Add(Id);
				double CLat, CLon; CellCentre(Id, CellM, CLat, CLon);
				const double D = GroundDistanceM(LatDeg, LonDeg, CLat, CLon);
				if (D <= RadiusM) Hits.Emplace(D, Id);
			}
		}
		Hits.Sort([](const TPair<double, FVegCellId>& A, const TPair<double, FVegCellId>& B)
		{
			if (A.Key != B.Key) return A.Key < B.Key;
			return A.Value.Row != B.Value.Row ? A.Value.Row < B.Value.Row : A.Value.Col < B.Value.Col;
		});
		for (const auto& H : Hits) Out.Add(H.Value);
		return Out;
	}
}
