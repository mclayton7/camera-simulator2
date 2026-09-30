// Copyright CamSim Contributors. All Rights Reserved.

#include "Ocean/OceanMeshBuilder.h"
#include "Ocean/OceanSurface.h"
#include "Geospatial/CigiFrames.h"

namespace CamSimOcean
{
	namespace
	{
		// Centre cell for a given alpha: d(1/(N/2)).
		double CentreCell(double R, double A, int32 N)
		{
			const double U = 1.0 / (N / 2);
			return A < 1e-9 ? R * U : R * (FMath::Exp(A * U) - 1.0) / (FMath::Exp(A) - 1.0);
		}
	}

	double SolveWarpAlpha(double RadiusM, int32 N, double InCentreCellM)
	{
		if (RadiusM / (N / 2) <= InCentreCellM) return 0.0;        // uniform grid is already fine enough
		double Lo = 1e-6, Hi = 60.0;                               // centre cell decreases with alpha
		for (int32 It = 0; It < 100; ++It)
		{
			const double Mid = 0.5 * (Lo + Hi);
			if (CentreCell(RadiusM, Mid, N) > InCentreCellM) Lo = Mid; else Hi = Mid;
		}
		return 0.5 * (Lo + Hi);
	}

	double WarpDistance(double U, double RadiusM, double Alpha)
	{
		const double A = FMath::Abs(U);
		const double D = Alpha < 1e-9 ? RadiusM * A : RadiusM * (FMath::Exp(Alpha * A) - 1.0) / (FMath::Exp(Alpha) - 1.0);
		return U < 0.0 ? -D : D;
	}

	double HorizonRadiusM(double CentreToNadirM, double AltAboveSeaM, double MaxRadiusKm)
	{
		return FMath::Min(CentreToNadirM + HorizonDistanceM(AltAboveSeaM), MaxRadiusKm * 1000.0);
	}

	double HorizonDistanceM(double AltAboveSeaM)
	{
		return 3570.0 * FMath::Sqrt(FMath::Max(AltAboveSeaM, 2.0)) * 1.1;
	}

	bool BuildOceanMesh(double CentreLat, double CentreLon, double RadiusM, const FOceanSurface& Ocean,
		const FGeoToWorldFn& GeoToWorld, FOceanMeshData& Out)
	{
		const TOptional<double> CentreSea = Ocean.SeaLevelM(CentreLat, CentreLon);
		if (!CentreSea.IsSet()) return false;

		Out = FOceanMeshData();
		Out.CentreLat = CentreLat; Out.CentreLon = CentreLon; Out.RadiusM = RadiusM;
		Out.Alpha = SolveWarpAlpha(RadiusM);
		Out.OriginWorld = GeoToWorld(CentreLat, CentreLon, *CentreSea);
		const FVector UpWorld = (GeoToWorld(CentreLat, CentreLon, *CentreSea + 1.0) - Out.OriginWorld).GetSafeNormal();

		const int32 V = GridN + 1;
		TArray<double> D; D.SetNumUninitialized(V);
		for (int32 i = 0; i < V; ++i) D[i] = WarpDistance(2.0 * i / GridN - 1.0, RadiusM, Out.Alpha);

		Out.Positions.SetNumUninitialized(V * V);
		Out.CellSize.SetNumUninitialized(V * V);
		Out.Normals.Init(UpWorld, V * V);
		for (int32 r = 0; r < V; ++r)            // r: north index
		for (int32 c = 0; c < V; ++c)            // c: east index
		{
			double Lat, Lon, Alt;
			CamSimFrames::OffsetGeodetic(CentreLat, CentreLon, 0.0, FVector(D[r], D[c], 0.0), Lat, Lon, Alt);
			const double Sea = Ocean.SeaLevelM(Lat, Lon).Get(*CentreSea);
			const int32 Idx = r * V + c;
			Out.Positions[Idx] = GeoToWorld(Lat, Lon, Sea) - Out.OriginWorld;
			// Average cell width over the neighbour cells actually spanned: 2 in the
			// interior (D[r+1]-D[r-1]) but only 1 at an edge (r=0 or r=GridN), where
			// only one neighbour cell exists, so dividing by 2 there would halve it.
			const int32 RLo = FMath::Max(r - 1, 0), RHi = FMath::Min(r + 1, GridN);
			const int32 CLo = FMath::Max(c - 1, 0), CHi = FMath::Min(c + 1, GridN);
			const double CellN = (D[RHi] - D[RLo]) / (RHi - RLo);
			const double CellE = (D[CHi] - D[CLo]) / (CHi - CLo);
			Out.CellSize[Idx] = FVector2D(FMath::Max(CellN, CellE), 0.0);   // metres; UV1.x in the material
		}
		Out.Triangles.Reserve(GridN * GridN * 6);
		for (int32 r = 0; r < GridN; ++r)
		for (int32 c = 0; c < GridN; ++c)
		{
			const int32 A = r * V + c, B = A + 1, C = A + V, Dd = C + 1;
			// Front faces up: A -> B (east) -> C (north). Verified in the running app (Task 9):
			// the reverse order (A, C, B) is back-face culled, i.e. the sea vanishes from above.
			Out.Triangles.Append({ A, B, C, B, Dd, C });
		}
		return true;
	}

	bool NeedsRebuild(const FRebuildPolicy& Last, double Lat, double Lon, double RadiusM)
	{
		if (!Last.bHasMesh) return true;
		const FVector Move = CamSimFrames::GeodeticDeltaToNeu(Last.LastLat, Last.LastLon, 0.0, Lat, Lon, 0.0);
		if (FMath::Sqrt(Move.X * Move.X + Move.Y * Move.Y) > 0.005 * Last.LastRadiusM) return true;
		return FMath::Abs(RadiusM - Last.LastRadiusM) > 0.25 * Last.LastRadiusM;
	}

	void ChooseCentre(double NadirLat, double NadirLon, double FcLat, double FcLon, bool bValid, double& OutLat, double& OutLon)
	{
		const bool bUse = bValid && FMath::IsFinite(FcLat) && FMath::IsFinite(FcLon) && FMath::Abs(FcLat) <= 90.0;
		OutLat = bUse ? FcLat : NadirLat;
		OutLon = bUse ? FcLon : NadirLon;
	}

	bool NeedsReanchor(bool bHasAnchor, double AnchorLat, double AnchorLon, double Lat, double Lon, bool bTeleport)
	{
		if (!bHasAnchor || bTeleport) return true;
		const FVector D = CamSimFrames::GeodeticDeltaToNeu(AnchorLat, AnchorLon, 0.0, Lat, Lon, 0.0);
		return D.X * D.X + D.Y * D.Y > FMath::Square(200000.0);
	}

	bool TideMoved(const FRebuildPolicy& Last, double TideM)
	{
		return Last.bHasMesh && FMath::Abs(TideM - Last.LastTideM) > TideRebuildM;
	}

	bool NeedsRebuild(const FRebuildPolicy& Last, double Lat, double Lon, double RadiusM, double SecondsSinceBuild, bool bForce)
	{
		if (!Last.bHasMesh || bForce) return true;
		if (SecondsSinceBuild < MinRebuildIntervalS) return false;
		return NeedsRebuild(Last, Lat, Lon, RadiusM);
	}

	void LimitCentreToMesh(double NadirLat, double NadirLon, double AltAboveSeaM, double MaxRadiusKm,
		double& InOutLat, double& InOutLon)
	{
		const FVector D = CamSimFrames::GeodeticDeltaToNeu(NadirLat, NadirLon, 0.0, InOutLat, InOutLon, 0.0);
		const double Limit = MaxRadiusKm * 1000.0 - HorizonDistanceM(AltAboveSeaM);
		if (D.X * D.X + D.Y * D.Y > FMath::Square(FMath::Max(Limit, 0.0)))
		{
			InOutLat = NadirLat;
			InOutLon = NadirLon;
		}
	}

	FCentreDecision UpdateCentreAndAnchor(FCentreTracker& Track, FOceanSurface& Ocean,
		double NadirLat, double NadirLon, double AltAboveSeaM, double FcLat, double FcLon, bool bFrameCentreValid,
		double MaxRadiusKm)
	{
		FCentreDecision Out;
		// Teleport: the nadir moved > 5 km since the last tick (no camera flies 150 km/s).
		const FVector Hop = CamSimFrames::GeodeticDeltaToNeu(Track.LastNadirLat, Track.LastNadirLon, 0.0, NadirLat, NadirLon, 0.0);
		Out.bTeleport = Track.bHasNadir && Hop.X * Hop.X + Hop.Y * Hop.Y > FMath::Square(5000.0);
		Track.LastNadirLat = NadirLat; Track.LastNadirLon = NadirLon; Track.bHasNadir = true;

		ChooseCentre(NadirLat, NadirLon, FcLat, FcLon, bFrameCentreValid, Out.Lat, Out.Lon);
		LimitCentreToMesh(NadirLat, NadirLon, AltAboveSeaM, MaxRadiusKm, Out.Lat, Out.Lon);
		const FOceanWaves& W = Ocean.GetWaves();
		if (NeedsReanchor(W.HasAnchor(), W.GetAnchorLat(), W.GetAnchorLon(), Out.Lat, Out.Lon, Out.bTeleport))
		{
			Ocean.SetAnchor(Out.Lat, Out.Lon);
			Out.bReanchored = true;
		}
		return Out;
	}
}
