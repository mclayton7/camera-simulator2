// Copyright CamSim Contributors. All Rights Reserved.

#include "Geospatial/Geoid.h"
#include "CamSimTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace CamSim::Geospatial
{
	// WW15MGH.DAC: 721 rows (90N..90S) x 1440 columns (0E..359.75E) at 0.25 deg,
	// big-endian int16 undulation in centimetres.
	static constexpr int32  GridRows    = 721;
	static constexpr int32  GridCols    = 1440;
	static constexpr double GridStepDeg = 0.25;

	FString GetGeoidGridPath()
	{
		// Content/NonUFS is staged as loose files (DefaultGame.ini
		// DirectoriesToAlwaysStageAsNonUFS), so this works packaged too.
		return FPaths::Combine(FPaths::ProjectContentDir(), TEXT("NonUFS/Geoid/WW15MGH.DAC"));
	}

	/** Undulations in metres, row-major; empty if the grid could not be loaded. */
	static TArray<float> LoadGrid()
	{
		const FString Path = GetGeoidGridPath();
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() < GridRows * GridCols * 2)
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("Geoid: no valid EGM96 grid at %s; KLV MSL altitudes (Tags 15/25) are omitted"), *Path);
			return {};
		}
		TArray<float> Grid;
		Grid.SetNumUninitialized(GridRows * GridCols);
		for (int32 i = 0; i < Grid.Num(); ++i)
		{
			const int16 Cm = static_cast<int16>((Bytes[2 * i] << 8) | Bytes[2 * i + 1]);
			Grid[i] = Cm / 100.0f;
		}
		return Grid;
	}

	TOptional<double> GetGeoidUndulation(double LatDeg, double LonDeg)
	{
		static const TArray<float> Grid = LoadGrid();
		if (Grid.IsEmpty() || !FMath::IsFinite(LatDeg) || !FMath::IsFinite(LonDeg))
		{
			return {};
		}

		// Bilinear. Columns wrap at 360E -> 0E (not clamped: Cesium Native's
		// EarthGravitationalModel1996Grid clamps there, which is wrong within
		// 0.25 deg west of the prime meridian).
		const double Y = (90.0 - FMath::Clamp(LatDeg, -90.0, 90.0)) / GridStepDeg;
		const double X = FMath::Fmod(FMath::Fmod(LonDeg, 360.0) + 360.0, 360.0) / GridStepDeg;
		const int32  Row = FMath::Min(FMath::FloorToInt32(Y), GridRows - 2);
		const int32  Col = FMath::FloorToInt32(X) % GridCols;
		const double Fy = Y - Row;
		const double Fx = X - FMath::FloorToDouble(X);
		auto At = [](int32 R, int32 C) { return static_cast<double>(Grid[R * GridCols + C % GridCols]); };
		const double North = FMath::Lerp(At(Row, Col),     At(Row, Col + 1),     Fx);
		const double South = FMath::Lerp(At(Row + 1, Col), At(Row + 1, Col + 1), Fx);
		return FMath::Lerp(North, South, Fy);
	}
}
