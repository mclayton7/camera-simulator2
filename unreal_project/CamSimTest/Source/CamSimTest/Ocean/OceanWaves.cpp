// Copyright CamSim Contributors. All Rights Reserved.

#include "Ocean/OceanWaves.h"
#include "Ocean/FBeaufortTable.h"
#include "Geospatial/EcefFrames.h"

namespace
{
	double Wrap2Pi(double X)
	{
		const double R = FMath::Fmod(X, 2.0 * PI);
		return R < 0.0 ? R + 2.0 * PI : R;
	}
}

TArray<FOceanWave> FOceanWaves::FromBeaufort(double Beaufort, double FromDeg, double Choppiness)
{
	const FBeaufortEntry E = FBeaufortTable::Sample(static_cast<float>(Beaufort));
	TArray<FOceanWave> Out;
	if (E.WaveHtM <= 0.0f || E.WaveLenM <= 0.0f) return Out;

	static constexpr double LenMul[MaxWaves] = { 0.6, 0.85, 1.1, 1.4 };
	static constexpr double DirOff[MaxWaves] = { -30.0, -10.0, 10.0, 30.0 };
	static constexpr double Weight[MaxWaves] = { 0.2, 0.35, 0.3, 0.15 };
	double SumW2 = 0.0;
	for (double W : Weight) SumW2 += W * W;
	// Hs = 4 sqrt(sum a_i^2 / 2) with a_i = s w_i
	const double S = E.WaveHtM / (4.0 * FMath::Sqrt(SumW2 / 2.0));
	for (int32 i = 0; i < MaxWaves; ++i)
	{
		FOceanWave W;
		W.HeightM  = 2.0 * S * Weight[i];
		W.LengthM  = E.WaveLenM * LenMul[i];
		W.FromDeg  = FromDeg + DirOff[i];
		W.PhaseRad = 1.7 * i;   // decorrelate the crests
		Out.Add(W);
	}
	AssignSteepness(Out, Choppiness);
	return Out;
}

void FOceanWaves::AssignSteepness(TArray<FOceanWave>& InWaves, double Choppiness)
{
	const double C = FMath::Clamp(Choppiness, 0.0, 1.0);
	int32 N = 0;
	for (const FOceanWave& W : InWaves) { if (W.HeightM > 0.0 && W.LengthM > 0.0) ++N; }
	for (FOceanWave& W : InWaves)
	{
		const double KA = (W.LengthM > 0.0) ? (2.0 * PI / W.LengthM) * W.HeightM * 0.5 : 0.0;
		W.Steepness = (KA > 0.0 && N > 0) ? C / (KA * N) : 0.0;
	}
}

void FOceanWaves::SetWaves(const TArray<FOceanWave>& InWaves)
{
	Waves.Reset();
	for (const FOceanWave& W : InWaves)
	{
		const bool bValid = FMath::IsFinite(W.HeightM) && FMath::IsFinite(W.LengthM) && W.HeightM > 0.0 && W.LengthM > 0.0
			&& FMath::IsFinite(W.PeriodS) && W.PeriodS >= 0.0 && FMath::IsFinite(W.FromDeg) && FMath::IsFinite(W.PhaseRad)
			&& FMath::IsFinite(W.Steepness);
		if (bValid && Waves.Num() < MaxWaves) Waves.Add(W);
	}
}

void FOceanWaves::SetAnchor(double Lat, double Lon)
{
	AnchorLat = Lat; AnchorLon = Lon; bHasAnchor = true;
	AnchorEcef = CamSimFrames::GeodeticToEcef(Lat, Lon, 0.0);
	const double La = FMath::DegreesToRadians(Lat), Lo = FMath::DegreesToRadians(Lon);
	AxisE = FVector(-FMath::Sin(Lo), FMath::Cos(Lo), 0.0);
	AxisN = FVector(-FMath::Sin(La) * FMath::Cos(Lo), -FMath::Sin(La) * FMath::Sin(Lo), FMath::Cos(La));
	AxisU = FVector(FMath::Cos(La) * FMath::Cos(Lo), FMath::Cos(La) * FMath::Sin(Lo), FMath::Sin(La));
}

double FOceanWaves::WaveNumber(int32 i) const { return 2.0 * PI / Waves[i].LengthM; }

double FOceanWaves::AngularFrequency(int32 i) const
{
	return Waves[i].PeriodS > 0.0 ? 2.0 * PI / Waves[i].PeriodS : FMath::Sqrt(G * WaveNumber(i));
}

FVector2D FOceanWaves::TravelDir(int32 i) const
{
	const double B = FMath::DegreesToRadians(Waves[i].FromDeg + 180.0);
	FVector2D D(FMath::Cos(B), FMath::Sin(B));
	if (FMath::Abs(D.X) < 1e-15) D.X = 0.0;
	if (FMath::Abs(D.Y) < 1e-15) D.Y = 0.0;
	return D;
}

double FOceanWaves::Phase(int32 i) const
{
	return Wrap2Pi(Wrap2Pi(Waves[i].PhaseRad) - Wrap2Pi(AngularFrequency(i) * TimeS));
}

FVector2D FOceanWaves::PlaneCoords(double Lat, double Lon, double AltM) const
{
	const FVector Rel = CamSimFrames::GeodeticToEcef(Lat, Lon, AltM) - AnchorEcef;
	return FVector2D(FVector::DotProduct(Rel, AxisN), FVector::DotProduct(Rel, AxisE));
}

FVector FOceanWaves::Displacement(double N, double E) const
{
	FVector D = FVector::ZeroVector;
	for (int32 i = 0; i < Waves.Num(); ++i)
	{
		const FVector2D Dir = TravelDir(i);
		const double a = Amplitude(i), Th = WaveNumber(i) * (Dir.X * N + Dir.Y * E) + Phase(i);
		const double S = FMath::Sin(Th), C = FMath::Cos(Th), Qa = Waves[i].Steepness * a;
		D.X -= Qa * Dir.X * S;
		D.Y -= Qa * Dir.Y * S;
		D.Z += a * C;
	}
	return D;
}

FVector2D FOceanWaves::Invert(double N, double E) const
{
	FVector2D P(N, E);
	for (int32 It = 0; It < 8; ++It)
	{
		const FVector D = Displacement(P.X, P.Y);
		const FVector2D Next(N - D.X, E - D.Y);
		const bool bDone = FVector2D::DistSquared(Next, P) < 1e-6;
		P = Next;
		if (bDone) break;
	}
	return P;
}

double FOceanWaves::HeightAtPlane(double N, double E) const
{
	if (Waves.Num() == 0) return 0.0;
	const FVector2D P = Invert(N, E);
	return Displacement(P.X, P.Y).Z;
}

FVector FOceanWaves::NormalAtParam(double N, double E) const
{
	FVector Nrm(0.0, 0.0, 1.0);
	for (int32 i = 0; i < Waves.Num(); ++i)
	{
		const FVector2D Dir = TravelDir(i);
		const double k = WaveNumber(i), a = Amplitude(i), Th = k * (Dir.X * N + Dir.Y * E) + Phase(i);
		const double KA = k * a;
		Nrm.X += Dir.X * KA * FMath::Sin(Th);
		Nrm.Y += Dir.Y * KA * FMath::Sin(Th);
		Nrm.Z -= Waves[i].Steepness * KA * FMath::Cos(Th);
	}
	return Nrm.GetSafeNormal();
}

FVector FOceanWaves::NormalAtPlane(double N, double E) const
{
	if (Waves.Num() == 0) return FVector(0.0, 0.0, 1.0);
	const FVector2D P = Invert(N, E);
	return NormalAtParam(P.X, P.Y);
}

double FOceanWaves::HeightAt(double Lat, double Lon, double SeaAltM) const
{
	const FVector2D P = PlaneCoords(Lat, Lon, SeaAltM);
	return HeightAtPlane(P.X, P.Y);
}

FVector FOceanWaves::NormalAt(double Lat, double Lon, double SeaAltM) const
{
	const FVector2D P = PlaneCoords(Lat, Lon, SeaAltM);
	return NormalAtPlane(P.X, P.Y);
}

double FOceanWaves::SignificantHeight() const
{
	double S = 0.0;
	for (int32 i = 0; i < Waves.Num(); ++i) S += FMath::Square(Amplitude(i)) / 2.0;
	return 4.0 * FMath::Sqrt(S);
}
