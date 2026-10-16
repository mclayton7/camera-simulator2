// Copyright CamSim Contributors. All Rights Reserved.

#include "Environment/Ephemeris.h"

namespace
{
	constexpr double DegToRad = UE_DOUBLE_PI / 180.0;
	constexpr double RadToDeg = 180.0 / UE_DOUBLE_PI;
	constexpr double AuKm = 149597870.7;
	constexpr double EarthRadiusKm = 6378.14;   // Meeus ch. 40
	constexpr double EarthBOverA = 0.99664719;  // WGS-84 polar / equatorial radius

	double Norm360(double Deg)
	{
		const double R = FMath::Fmod(Deg, 360.0);
		return R < 0.0 ? R + 360.0 : R;
	}
	double SinD(double Deg) { return FMath::Sin(Deg * DegToRad); }
	double CosD(double Deg) { return FMath::Cos(Deg * DegToRad); }

	// Meeus table 47.A: periodic terms for the moon's longitude (Σl, 1e-6 deg) and distance (Σr, 1e-3 km).
	struct FMoonLrTerm { int8 D, M, Mp, F; int32 L, R; };
	static const FMoonLrTerm MoonLrTerms[] = {
		{0, 0, 1, 0, 6288774, -20905355}, {2, 0, -1, 0, 1274027, -3699111},
		{2, 0, 0, 0, 658314, -2955968},   {0, 0, 2, 0, 213618, -569925},
		{0, 1, 0, 0, -185116, 48888},     {0, 0, 0, 2, -114332, -3149},
		{2, 0, -2, 0, 58793, 246158},     {2, -1, -1, 0, 57066, -152138},
		{2, 0, 1, 0, 53322, -170733},     {2, -1, 0, 0, 45758, -204586},
		{0, 1, -1, 0, -40923, -129620},   {1, 0, 0, 0, -34720, 108743},
		{0, 1, 1, 0, -30383, 104755},     {2, 0, 0, -2, 15327, 10321},
		{0, 0, 1, 2, -12528, 0},          {0, 0, 1, -2, 10980, 79661},
		{4, 0, -1, 0, 10675, -34782},     {0, 0, 3, 0, 10034, -23210},
		{4, 0, -2, 0, 8548, -21636},      {2, 1, -1, 0, -7888, 24208},
		{2, 1, 0, 0, -6766, 30824},       {1, 0, -1, 0, -5163, -8379},
		{1, 1, 0, 0, 4987, -16675},       {2, -1, 1, 0, 4036, -12831},
		{2, 0, 2, 0, 3994, -10445},       {4, 0, 0, 0, 3861, -11650},
		{2, 0, -3, 0, 3665, 14403},       {0, 1, -2, 0, -2689, -7003},
		{2, 0, -1, 2, -2602, 0},          {2, -1, -2, 0, 2390, 10056},
		{1, 0, 1, 0, -2348, 6322},        {2, -2, 0, 0, 2236, -9884},
		{0, 1, 2, 0, -2120, 5751},        {0, 2, 0, 0, -2069, 0},
		{2, -2, -1, 0, 2048, -4950},      {2, 0, 1, -2, -1773, 4130},
		{2, 0, 0, 2, -1595, 0},           {4, -1, -1, 0, 1215, -3958},
		{0, 0, 2, 2, -1110, 0},           {3, 0, -1, 0, -892, 3258},
		{2, 1, 1, 0, -810, 2616},         {4, -1, -2, 0, 759, -1897},
		{0, 2, -1, 0, -713, -2117},       {2, 2, -1, 0, -700, 2354},
		{2, 1, -2, 0, 691, 0},            {2, -1, 0, -2, 596, 0},
		{4, 0, 1, 0, 549, -1423},         {0, 0, 4, 0, 537, -1117},
		{4, -1, 0, 0, 520, -1571},        {1, 0, -2, 0, -487, -1739},
		{2, 1, 0, -2, -399, 0},           {0, 0, 2, -2, -381, -4421},
		{1, 1, 1, 0, 351, 0},             {3, 0, -2, 0, -340, 0},
		{4, 0, -3, 0, 330, 0},            {2, -1, 2, 0, 327, 0},
		{0, 2, 1, 0, -323, 1165},         {1, 1, -1, 0, 299, 0},
		{2, 0, 3, 0, 294, 0},             {2, 0, -1, -2, 0, 8752},
	};
	static_assert(UE_ARRAY_COUNT(MoonLrTerms) == 60, "Meeus table 47.A has 60 rows");

	// Meeus table 47.B: periodic terms for the moon's latitude (Σb, 1e-6 deg).
	struct FMoonBTerm { int8 D, M, Mp, F; int32 B; };
	static const FMoonBTerm MoonBTerms[] = {
		{0, 0, 0, 1, 5128122}, {0, 0, 1, 1, 280602},  {0, 0, 1, -1, 277693}, {2, 0, 0, -1, 173237},
		{2, 0, -1, 1, 55413},  {2, 0, -1, -1, 46271}, {2, 0, 0, 1, 32573},   {0, 0, 2, 1, 17198},
		{2, 0, 1, -1, 9266},   {0, 0, 2, -1, 8822},   {2, -1, 0, -1, 8216},  {2, 0, -2, -1, 4324},
		{2, 0, 1, 1, 4200},    {2, 1, 0, -1, -3359},  {2, -1, -1, 1, 2463},  {2, -1, 0, 1, 2211},
		{2, -1, -1, -1, 2065}, {0, 1, -1, -1, -1870}, {4, 0, -1, -1, 1828},  {0, 1, 0, 1, -1794},
		{0, 0, 0, 3, -1749},   {0, 1, -1, 1, -1565},  {1, 0, 0, 1, -1491},   {0, 1, 1, 1, -1475},
		{0, 1, 1, -1, -1410},  {0, 1, 0, -1, -1344},  {1, 0, 0, -1, -1335},  {0, 0, 3, 1, 1107},
		{4, 0, 0, -1, 1021},   {4, 0, -1, 1, 833},    {0, 0, 1, -3, 777},    {4, 0, -2, 1, 671},
		{2, 0, 0, -3, 607},    {2, 0, 2, -1, 596},    {2, -1, 1, -1, 491},   {2, 0, -2, 1, -451},
		{0, 0, 3, -1, 439},    {2, 0, 2, 1, 422},     {2, 0, -3, -1, 421},   {2, 1, -1, 1, -366},
		{2, 1, 0, 1, -351},    {4, 0, 0, 1, 331},     {2, -1, 1, 1, 315},    {2, -2, 0, -1, 302},
		{0, 0, 1, 3, -283},    {2, 1, 1, -1, -229},   {1, 1, 0, -1, 223},    {1, 1, 0, 1, 223},
		{0, 1, -2, -1, -220},  {2, 1, -1, -1, -220},  {1, 0, 1, 1, -185},    {2, -1, -2, -1, 181},
		{0, 1, 2, 1, -177},    {4, 0, -2, -1, 176},   {4, -1, -1, -1, 166},  {1, 0, 1, -1, -164},
		{4, 0, 1, -1, 132},    {1, 0, -1, -1, -119},  {4, -1, 0, -1, 115},   {2, -2, 0, 1, 107},
	};
	static_assert(UE_ARRAY_COUNT(MoonBTerms) == 60, "Meeus table 47.B has 60 rows");

	// E factor for terms in the sun's mean anomaly M (Meeus ch. 47): E for |M| = 1, E² for |M| = 2.
	double EccentricityFactor(int32 MCoeff, double E)
	{
		const int32 A = FMath::Abs(MCoeff);
		return A == 1 ? E : (A == 2 ? E * E : 1.0);
	}

	/** Ecliptic (λ, β) → equatorial (α, δ), all degrees. */
	void EclipticToEquatorial(double LambdaDeg, double BetaDeg, double EpsDeg, double& OutRaDeg, double& OutDecDeg)
	{
		const double SinL = SinD(LambdaDeg), CosL = CosD(LambdaDeg);
		const double SinB = SinD(BetaDeg), CosB = CosD(BetaDeg);
		const double SinE = SinD(EpsDeg), CosE = CosD(EpsDeg);
		OutRaDeg = Norm360(FMath::Atan2(SinL * CosE * CosB - SinB * SinE, CosL * CosB) * RadToDeg);
		OutDecDeg = FMath::Asin(FMath::Clamp(SinB * CosE + CosB * SinE * SinL, -1.0, 1.0)) * RadToDeg;
	}

	/** Hour angle / declination → compass azimuth and elevation at geodetic latitude φ (degrees). */
	void EquatorialToHorizontal(double HourAngleDeg, double DecDeg, double LatDeg, double& OutAzDeg, double& OutElDeg)
	{
		const double SinH = SinD(HourAngleDeg), CosH = CosD(HourAngleDeg);
		const double SinD_ = SinD(DecDeg), CosD_ = CosD(DecDeg);
		const double SinP = SinD(LatDeg), CosP = CosD(LatDeg);
		OutElDeg = FMath::Asin(FMath::Clamp(SinP * SinD_ + CosP * CosD_ * CosH, -1.0, 1.0)) * RadToDeg;
		OutAzDeg = Norm360(FMath::Atan2(-SinH * CosD_, CosP * SinD_ - SinP * CosD_ * CosH) * RadToDeg);
	}
}

double FEphemeris::JulianDayUtc(const FDateTime& Utc)
{
	return Utc.GetJulianDay();
}

double FEphemeris::DeltaTSeconds(int32 Year, int32 Month)
{
	const double Y = FMath::Clamp(Year + (Month - 0.5) / 12.0, 1986.0, 2050.0);
	const double T = Y - 2000.0;
	if (Y >= 2005.0)
	{
		return 62.92 + 0.32217 * T + 0.005589 * T * T;
	}
	return 63.86 + 0.3345 * T - 0.060374 * T * T + 0.0017275 * T * T * T + 0.000651814 * T * T * T * T
		+ 0.00002373599 * T * T * T * T * T;
}

FVector3d FEphemeris::AzElToEnu(double AzDeg, double ElDeg)
{
	const double CosEl = CosD(ElDeg);
	return FVector3d(SinD(AzDeg) * CosEl, CosD(AzDeg) * CosEl, SinD(ElDeg));
}

FEphemerisResult FEphemeris::Compute(const FDateTime& Utc, double LatDeg, double LonDeg, double HeightM)
{
	FEphemerisResult Out;

	const double JdUt = JulianDayUtc(Utc);
	const double Jde = JdUt + DeltaTSeconds(Utc.GetYear(), Utc.GetMonth()) / 86400.0;
	const double T = (Jde - 2451545.0) / 36525.0;
	const double T2 = T * T, T3 = T2 * T, T4 = T3 * T;

	// Nutation and obliquity (Meeus ch. 22, low accuracy).
	const double Omega = 125.04452 - 1934.136261 * T;
	const double LSun = 280.4665 + 36000.7698 * T;
	const double LMoon = 218.3165 + 481267.8813 * T;
	const double DPsiDeg = (-17.20 * SinD(Omega) - 1.32 * SinD(2 * LSun) - 0.23 * SinD(2 * LMoon) + 0.21 * SinD(2 * Omega)) / 3600.0;
	const double DEpsDeg = (9.20 * CosD(Omega) + 0.57 * CosD(2 * LSun) + 0.10 * CosD(2 * LMoon) - 0.09 * CosD(2 * Omega)) / 3600.0;
	const double Eps0 = 23.0 + 26.0 / 60.0 + 21.448 / 3600.0 - 46.8150 / 3600.0 * T;
	const double EpsTrue = Eps0 + DEpsDeg;

	// Sun (Meeus ch. 25, low accuracy).
	double SunRa = 0, SunDec = 0, SunRAu = 1;
	{
		const double L0 = 280.46646 + 36000.76983 * T + 0.0003032 * T2;
		const double M = 357.52911 + 35999.05029 * T - 0.0001537 * T2;
		const double Ecc = 0.016708634 - 0.000042037 * T;
		const double C = (1.914602 - 0.004817 * T - 0.000014 * T2) * SinD(M) + (0.019993 - 0.000101 * T) * SinD(2 * M)
			+ 0.000289 * SinD(3 * M);
		const double TrueLon = L0 + C;
		const double Nu = M + C;
		SunRAu = 1.000001018 * (1.0 - Ecc * Ecc) / (1.0 + Ecc * CosD(Nu));
		const double OmegaS = 125.04 - 1934.136 * T;
		const double Lambda = TrueLon - 0.00569 - 0.00478 * SinD(OmegaS);
		const double Eps = Eps0 + 0.00256 * CosD(OmegaS);
		EclipticToEquatorial(Lambda, 0.0, Eps, SunRa, SunDec);
	}

	// Moon (Meeus ch. 47), geocentric.
	double MoonRa = 0, MoonDec = 0, MoonDistKm = 385000.56;
	{
		const double Lp = 218.3164477 + 481267.88123421 * T - 0.0015786 * T2 + T3 / 538841.0 - T4 / 65194000.0;
		const double D = 297.8501921 + 445267.1114034 * T - 0.0018819 * T2 + T3 / 545868.0 - T4 / 113065000.0;
		const double M = 357.5291092 + 35999.0502909 * T - 0.0001536 * T2 + T3 / 24490000.0;
		const double Mp = 134.9633964 + 477198.8675055 * T + 0.0087414 * T2 + T3 / 69699.0 - T4 / 14712000.0;
		const double F = 93.2720950 + 483202.0175233 * T - 0.0036539 * T2 - T3 / 3526000.0 + T4 / 863310000.0;
		const double A1 = 119.75 + 131.849 * T;
		const double A2 = 53.09 + 479264.290 * T;
		const double A3 = 313.45 + 481266.484 * T;
		const double E = 1.0 - 0.002516 * T - 0.0000074 * T2;

		double SumL = 0, SumR = 0, SumB = 0;
		for (const FMoonLrTerm& Term : MoonLrTerms)
		{
			const double Arg = Term.D * D + Term.M * M + Term.Mp * Mp + Term.F * F;
			const double EF = EccentricityFactor(Term.M, E);
			SumL += Term.L * EF * SinD(Arg);
			SumR += Term.R * EF * CosD(Arg);
		}
		for (const FMoonBTerm& Term : MoonBTerms)
		{
			const double Arg = Term.D * D + Term.M * M + Term.Mp * Mp + Term.F * F;
			SumB += Term.B * EccentricityFactor(Term.M, E) * SinD(Arg);
		}
		SumL += 3958.0 * SinD(A1) + 1962.0 * SinD(Lp - F) + 318.0 * SinD(A2);
		SumB += -2235.0 * SinD(Lp) + 382.0 * SinD(A3) + 175.0 * SinD(A1 - F) + 175.0 * SinD(A1 + F)
			+ 127.0 * SinD(Lp - Mp) - 115.0 * SinD(Lp + Mp);

		const double Lambda = Lp + SumL / 1e6 + DPsiDeg; // apparent longitude
		const double Beta = SumB / 1e6;
		MoonDistKm = 385000.56 + SumR / 1000.0;
		EclipticToEquatorial(Lambda, Beta, EpsTrue, MoonRa, MoonDec);
	}

	// Local apparent sidereal time (Meeus 12.4 + equation of the equinoxes). UT1 ≈ UTC.
	{
		const double Tu = (JdUt - 2451545.0) / 36525.0;
		const double Theta0 = 280.46061837 + 360.98564736629 * (JdUt - 2451545.0) + 0.000387933 * Tu * Tu
			- Tu * Tu * Tu / 38710000.0;
		Out.LocalSiderealDeg = Norm360(Theta0 + DPsiDeg * CosD(EpsTrue) + LonDeg);
	}

	// Sun: geocentric is enough (parallax ≤ 8.8″).
	EquatorialToHorizontal(Out.LocalSiderealDeg - SunRa, SunDec, LatDeg, Out.SunAzDeg, Out.SunElDeg);

	// Moon topocentric (Meeus ch. 40), done as a vector difference in the true equatorial frame of date:
	// equivalent to the Δα / δ′ formulas with π = asin(6378.14 / Δ), and also gives the observer's distance.
	{
		const double U = FMath::Atan2(EarthBOverA * SinD(LatDeg), CosD(LatDeg));
		const double HRatio = HeightM / (EarthRadiusKm * 1000.0);
		const double RhoSinPhi = EarthBOverA * FMath::Sin(U) + HRatio * SinD(LatDeg);
		const double RhoCosPhi = FMath::Cos(U) + HRatio * CosD(LatDeg);
		const double Theta = Out.LocalSiderealDeg;
		const FVector3d Observer = EarthRadiusKm * FVector3d(RhoCosPhi * CosD(Theta), RhoCosPhi * SinD(Theta), RhoSinPhi);
		const FVector3d MoonGeo = MoonDistKm * FVector3d(CosD(MoonDec) * CosD(MoonRa), CosD(MoonDec) * SinD(MoonRa), SinD(MoonDec));
		const FVector3d Topo = MoonGeo - Observer;
		const double TopoKm = Topo.Length();
		const double TopoRa = FMath::Atan2(Topo.Y, Topo.X) * RadToDeg;
		const double TopoDec = FMath::Asin(FMath::Clamp(Topo.Z / TopoKm, -1.0, 1.0)) * RadToDeg;
		EquatorialToHorizontal(Out.LocalSiderealDeg - TopoRa, TopoDec, LatDeg, Out.MoonAzDeg, Out.MoonElDeg);
		Out.MoonDistanceKm = TopoKm;
	}

	// Phase (Meeus ch. 48), geocentric.
	{
		const double CosPsi = SinD(SunDec) * SinD(MoonDec) + CosD(SunDec) * CosD(MoonDec) * CosD(SunRa - MoonRa);
		const double Psi = FMath::Acos(FMath::Clamp(CosPsi, -1.0, 1.0));
		const double SunKm = SunRAu * AuKm;
		const double I = FMath::Atan2(SunKm * FMath::Sin(Psi), MoonDistKm - SunKm * FMath::Cos(Psi));
		Out.MoonPhaseAngleDeg = I * RadToDeg;
		Out.MoonIlluminatedFraction = (1.0 + FMath::Cos(I)) * 0.5;
	}

	Out.SunDistanceAu = SunRAu;
	Out.SunEnu = AzElToEnu(Out.SunAzDeg, Out.SunElDeg);
	Out.MoonEnu = AzElToEnu(Out.MoonAzDeg, Out.MoonElDeg);
	return Out;
}
