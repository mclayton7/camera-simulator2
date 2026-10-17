// Copyright CamSim Contributors. All Rights Reserved.
// The moon disc's surface (ROADMAP 6A defect 1, docs/night.md "Moon disc"): albedo map, IAU orientation and the
// albedo-weighted normalisation the dome uses (Environment/MoonSurface.h).

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Environment/MoonSurface.h"
#include "Environment/NightIllumination.h"
#include "Environment/SkyDomeDriver.h"

namespace
{
	/** Observer looks along +X at the moon; the sun at phase angle Alpha; the near side faces the observer, rotated by a
	 *  libration (LonLib, LatLib in degrees: the sub-observer point). */
	FMoonDiscFrame TestFrame(double AlphaDeg, double LonLib = 0.0, double LatLib = 0.0)
	{
		FMoonDiscFrame F;
		const double A = FMath::DegreesToRadians(AlphaDeg);
		F.MoonDir = FVector3d(1, 0, 0);
		F.SunDir = FVector3d(-FMath::Cos(A), FMath::Sin(A), 0.0);
		// Body axes with the sub-observer point at (LonLib, LatLib): the direction to the observer, -X, has body
		// coordinates (cos b cos l, cos b sin l, sin b).
		const FQuat4d Q = FQuat4d(FVector3d::UnitZ(), FMath::DegreesToRadians(-LonLib)) * FQuat4d(FVector3d::UnitY(), FMath::DegreesToRadians(LatLib));
		const FVector3d X0(-1, 0, 0), Y0(0, -1, 0), Z0(0, 0, 1);
		F.Axes[0] = Q.RotateVector(X0);
		F.Axes[1] = Q.RotateVector(Y0);
		F.Axes[2] = Q.RotateVector(Z0);
		F.L = FMoonDisc::LunarLambertL(AlphaDeg);
		return F;
	}

	FMoonAlbedoMap ShippedOrSynthetic(FAutomationTestBase& T)
	{
		FMoonAlbedoMap Map = FMoonAlbedoMap::Load(FMoonAlbedoMap::DefaultPath());
		if (Map.IsEmpty())
		{
			T.AddWarning(TEXT("moon_albedo.pgm not loaded (git lfs pull): using a synthetic map"));
			Map.Width = 64; Map.Height = 32;
			for (int32 I = 0; I < 64 * 32; ++I) Map.Texels.Add(static_cast<uint8>(60 + (I * 37) % 190));
		}
		return Map;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoonAlbedoPgmTest, "CamSim.Environment.MoonSurface.AlbedoPgm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMoonAlbedoPgmTest::RunTest(const FString&)
{
	auto Bytes = [](const char* Header, int32 N, uint8 Fill)
	{
		TArray<uint8> B;
		B.Append(reinterpret_cast<const uint8*>(Header), FCStringAnsi::Strlen(Header));
		for (int32 I = 0; I < N; ++I) B.Add(static_cast<uint8>(Fill + I));
		return B;
	};
	FString Err;
	const FMoonAlbedoMap M = FMoonAlbedoMap::ParsePgm(Bytes("P5\n# moon\n4 2\n255\n", 8, 10), Err);
	TestTrue(TEXT("4 x 2 parsed"), M.Width == 4 && M.Height == 2 && M.Texels.Num() == 8 && M.Texels[7] == 17);
	TestTrue(TEXT("P2 rejected"), FMoonAlbedoMap::ParsePgm(Bytes("P2\n4 2\n255\n", 8, 0), Err).IsEmpty());
	TestTrue(TEXT("16-bit rejected"), FMoonAlbedoMap::ParsePgm(Bytes("P5\n4 2\n65535\n", 16, 0), Err).IsEmpty());
	TestTrue(TEXT("truncated rejected"), FMoonAlbedoMap::ParsePgm(Bytes("P5\n4 2\n255\n", 7, 0), Err).IsEmpty());
	// Sampling: texel centres exact, longitude wraps (the left and right columns blend across +-180), empty = 1.
	FMoonAlbedoMap S;
	S.Width = 4; S.Height = 2; S.Texels = { 0, 51, 102, 255, 0, 51, 102, 255 };
	const double LonCentre1 = (1.5 / 4.0 - 0.5) * 2.0 * UE_DOUBLE_PI, LatRow0 = (0.5 - 0.5 / 2.0) * UE_DOUBLE_PI;
	TestTrue(TEXT("texel centre"), FMath::IsNearlyEqual(S.Sample(LonCentre1, LatRow0), 51.0 / 255.0, 1e-9));
	TestTrue(TEXT("wraps at 180 deg"), FMath::IsNearlyEqual(S.Sample(UE_DOUBLE_PI, LatRow0), 0.5 * (0.0 + 255.0) / 255.0, 1e-9));
	TestTrue(TEXT("latitude clamps"), FMath::IsNearlyEqual(S.Sample(LonCentre1, UE_DOUBLE_PI / 2.0), 51.0 / 255.0, 1e-9));
	TestEqual(TEXT("empty map is uniform"), FMoonAlbedoMap().Sample(0.3, 0.2), 1.0);
	return true;
}

// Meeus, Astronomical Algorithms ex. 53.a (1992 April 12, 0h TD; the moon of ex. 47.a: apparent RA 134.688470,
// Dec 13.768368): total libration l = -1.23, b = +4.20 deg, position angle of the axis P = 15.08 deg.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoonOrientationMeeusTest, "CamSim.Environment.MoonSurface.IauOrientationMeeus53a",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMoonOrientationMeeusTest::RunTest(const FString&)
{
	const double D = 2448724.5 - 2451545.0;
	const double Ra = FMath::DegreesToRadians(134.688470), Dec = FMath::DegreesToRadians(13.768368);
	const FVector3d MoonDate(FMath::Cos(Dec) * FMath::Cos(Ra), FMath::Cos(Dec) * FMath::Sin(Ra), FMath::Sin(Dec));
	FVector P[3];
	FSkyDomeDriver::PrecessionJ2000ToDate(D / 36525.0, P);   // rows: J2000 -> date
	auto ToJ2000 = [&P](const FVector3d& V) { return FVector3d(P[0].X * V.X + P[1].X * V.Y + P[2].X * V.Z,
		P[0].Y * V.X + P[1].Y * V.Y + P[2].Y * V.Z, P[0].Z * V.X + P[1].Z * V.Y + P[2].Z * V.Z); };
	const FVector3d Moon = ToJ2000(MoonDate);
	FVector3d Axes[3];
	FMoonOrientation::IauAxesJ2000(D, Axes);
	const FVector2d Sub = FMoonOrientation::SubPointDeg(Axes, -Moon);
	AddInfo(FString::Printf(TEXT("sub-Earth point l %.3f b %.3f (Meeus -1.23, +4.20)"), Sub.X, Sub.Y));
	TestTrue(TEXT("l within 0.15 deg"), FMath::Abs(Sub.X - (-1.23)) < 0.15);
	TestTrue(TEXT("b within 0.15 deg"), FMath::Abs(Sub.Y - 4.20) < 0.15);
	// Position angle of the axis (from celestial north toward east), in the J2000 frame of the moon's direction.
	const double RaJ = FMath::Atan2(Moon.Y, Moon.X), DecJ = FMath::Asin(Moon.Z);
	const FVector3d North(-FMath::Sin(DecJ) * FMath::Cos(RaJ), -FMath::Sin(DecJ) * FMath::Sin(RaJ), FMath::Cos(DecJ));
	const FVector3d East(-FMath::Sin(RaJ), FMath::Cos(RaJ), 0.0);
	const double Pa = FMath::RadiansToDegrees(FMath::Atan2(Axes[2].Dot(East), Axes[2].Dot(North)));
	AddInfo(FString::Printf(TEXT("position angle of the axis %.3f (Meeus 15.08)"), Pa));
	TestTrue(TEXT("P within 0.3 deg"), FMath::Abs(Pa - 15.08) < 0.3);
	TestTrue(TEXT("axes orthonormal, right-handed"), FMath::IsNearlyEqual(Axes[0].Cross(Axes[1]).Dot(Axes[2]), 1.0, 1e-12));
	return true;
}

// The dome's normaliser (MeanRadiance at the driver's automatic grid) against a fine integral: the disc's integrated
// luminance equals the K&S illuminance within 1 % at several phases and librations, with the shipped albedo map.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoonDiscAlbedoNormaliserTest, "CamSim.Environment.MoonSurface.Normaliser",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMoonDiscAlbedoNormaliserTest::RunTest(const FString&)
{
	const FMoonAlbedoMap Map = ShippedOrSynthetic(*this);
	for (double Alpha : { 0.0, 5.0, 45.0, 90.0, 135.0 })
	{
		for (const FVector2d Lib : { FVector2d(0, 0), FVector2d(7, -6), FVector2d(-7, 6) })
		{
			const FMoonDiscFrame F = TestFrame(Alpha, Lib.X, Lib.Y);
			const double Coarse = FMoonDisc::MeanRadiance(F, Map);
			const double Fine = FMoonDisc::MeanRadiance(F, Map, 700);
			// E_disc = sum of L(p) dOmega = LumScale x Omega x Fine with LumScale = E / (Omega x Coarse)
			TestTrue(FString::Printf(TEXT("alpha %.0f lib (%.0f, %.0f): integral / E = %.4f"), Alpha, Lib.X, Lib.Y, Fine / Coarse),
				FMath::Abs(Fine / Coarse - 1.0) < 0.01);
		}
	}
	// Uniform albedo reduces to MeanShading.
	TestTrue(TEXT("uniform map: MeanRadiance = MeanShading (gibbous)"),
		FMath::IsNearlyEqual(FMoonDisc::MeanRadiance(TestFrame(50.0), FMoonAlbedoMap(), 400), FMoonDisc::MeanShading(50.0), 2e-3));
	return true;
}

// The pre-exposure cap's bound: no point of the shipped disc is brighter than MaxPeakToMean x its mean (the full moon is
// the worst case: at larger phases K&S falls faster than the peak-to-mean grows, NightIllumination's cap test).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoonDiscPeakBoundTest, "CamSim.Environment.MoonSurface.PeakToMeanBound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMoonDiscPeakBoundTest::RunTest(const FString&)
{
	const FMoonAlbedoMap Map = ShippedOrSynthetic(*this);
	double Worst = 0.0;
	for (const FVector2d Lib : { FVector2d(0, 0), FVector2d(8, 7), FVector2d(-8, -7), FVector2d(8, -7), FVector2d(-8, 7) })
	{
		const FMoonDiscFrame F = TestFrame(0.0, Lib.X, Lib.Y);
		const double Mean = FMoonDisc::MeanRadiance(F, Map, 300);
		double Peak = 0.0;
		const int32 N = 600;
		for (int32 J = 0; J < N; ++J) for (int32 I = 0; I < N; ++I)
		{
			Peak = FMath::Max(Peak, FMoonDisc::Radiance(F, Map, -1.0 + 2.0 * (I + 0.5) / N, -1.0 + 2.0 * (J + 0.5) / N));
		}
		Worst = FMath::Max(Worst, Peak / Mean);
	}
	AddInfo(FString::Printf(TEXT("worst peak / mean %.3f (bound %.2f)"), Worst, FMoonDisc::MaxPeakToMean));
	TestTrue(TEXT("peak / mean within the bound"), Worst <= FMoonDisc::MaxPeakToMean);
	return true;
}

// Maria contrast on the full disc: the near side's maria make the disc visibly non-uniform (night_check N3's threshold
// is set from this); the map's maria are dark where the atlases put them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoonDiscMariaTest, "CamSim.Environment.MoonSurface.MariaContrast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMoonDiscMariaTest::RunTest(const FString&)
{
	const FMoonAlbedoMap Map = FMoonAlbedoMap::Load(FMoonAlbedoMap::DefaultPath());
	if (Map.IsEmpty())
	{
		AddWarning(TEXT("moon_albedo.pgm not loaded (git lfs pull): skipped"));
		return true;
	}
	const FMoonDiscFrame F = TestFrame(0.0);
	const int32 N = 200;
	double Sum = 0.0, Sum2 = 0.0;
	int32 Count = 0;
	for (int32 J = 0; J < N; ++J) for (int32 I = 0; I < N; ++I)
	{
		const double U = -1.0 + 2.0 * (I + 0.5) / N, V = -1.0 + 2.0 * (J + 0.5) / N;
		if (U * U + V * V > 0.81) continue;
		const double R = FMoonDisc::Radiance(F, Map, U, V);
		Sum += R; Sum2 += R * R; ++Count;
	}
	const double Mean = Sum / Count, Cv = FMath::Sqrt(FMath::Max(Sum2 / Count - Mean * Mean, 0.0)) / Mean;
	AddInfo(FString::Printf(TEXT("full disc (r < 0.9) std / mean %.3f"), Cv));
	TestTrue(TEXT("maria contrast: std / mean > 0.2"), Cv > 0.2);
	// The point at selenographic (17N, 59E) is darker than the highlands at (20S, 20E): Crisium vs the southern uplands.
	const FVector2d Origin = FMoonOrientation::SubPointDeg(F.Axes, F.Axes[0]);
	TestTrue(TEXT("SubPointDeg of axis X is (0, 0)"), FMath::Abs(Origin.X) < 1e-9 && FMath::Abs(Origin.Y) < 1e-9);
	TestTrue(TEXT("Mare Crisium darker than the southern highlands"),
		Map.Sample(FMath::DegreesToRadians(59.0), FMath::DegreesToRadians(17.0)) < 0.6 * Map.Sample(FMath::DegreesToRadians(20.0), FMath::DegreesToRadians(-20.0)));
	return true;
}

// The normaliser cache (fix round): recomputes only when the phase, sub-observer or sub-solar point moves by more than
// FMoonNormaliserCache::ToleranceDeg, never for a rotation of the whole frame (the camera moving); its mean is the
// fresh one when it does.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoonNormaliserCacheTest, "CamSim.Environment.MoonSurface.NormaliserCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMoonNormaliserCacheTest::RunTest(const FString&)
{
	const FMoonAlbedoMap Map = ShippedOrSynthetic(*this);
	FMoonNormaliserCache Cache;
	bool bRe = false;
	const FMoonDiscFrame F0 = TestFrame(40.0, 2.0, -1.0);
	const double M0 = Cache.Get(F0, Map, &bRe);
	TestTrue(TEXT("first call computes"), bRe);
	TestTrue(TEXT("first mean = MeanRadiance"), FMath::IsNearlyEqual(M0, FMoonDisc::MeanRadiance(F0, Map), 1e-12));
	Cache.Get(F0, Map, &bRe);
	TestFalse(TEXT("same frame: cached"), bRe);
	// A rotation of everything (camera / UE axes): invariant.
	FMoonDiscFrame Rot = F0;
	const FQuat4d Q(FVector3d(0.3, -0.5, 0.8).GetSafeNormal(), 1.1);
	Rot.MoonDir = Q.RotateVector(F0.MoonDir); Rot.SunDir = Q.RotateVector(F0.SunDir);
	for (int32 K = 0; K < 3; ++K) Rot.Axes[K] = Q.RotateVector(F0.Axes[K]);
	Cache.Get(Rot, Map, &bRe);
	TestFalse(TEXT("whole-frame rotation: cached"), bRe);
	Cache.Get(TestFrame(40.02, 2.0, -1.0), Map, &bRe);
	TestFalse(TEXT("phase +0.02 deg: cached"), bRe);
	Cache.Get(TestFrame(40.1, 2.0, -1.0), Map, &bRe);
	TestTrue(TEXT("phase +0.1 deg: recomputed"), bRe);
	Cache.Get(TestFrame(40.1, 2.1, -1.0), Map, &bRe);
	TestTrue(TEXT("libration +0.1 deg: recomputed"), bRe);
	TestEqual(TEXT("three recomputes"), Cache.GetRecomputeCount(), 3);
	AddInfo(FString::Printf(TEXT("MeanRadiance %.3f ms per recompute (phase 40, 128^2 grid)"), Cache.GetTotalComputeMs() / Cache.GetRecomputeCount()));
	// Holding the mean within the tolerance: 0.05 deg of phase moves it < 0.2 %.
	const double A = FMoonDisc::MeanRadiance(TestFrame(90.0), Map, 300), B = FMoonDisc::MeanRadiance(TestFrame(90.05), Map, 300);
	TestTrue(FString::Printf(TEXT("0.05 deg of phase at quarter changes the mean by %.3f %%"), 100.0 * FMath::Abs(B / A - 1.0)), FMath::Abs(B / A - 1.0) < 0.002);
	return true;
}

// Orientation (fix round): a mirrored or rotated albedo map, or a sign error in W, must fail. Selenographic east
// (Mare Crisium) faces celestial west (IAU 1961 convention: Crisium on the western side of the sky), and the map's
// known pairs are dark/bright at mirrored longitudes (Mare Humorum 24S 39W vs 24S 39E, Mare Smythii 1N 87E vs 1N 87W,
// Aristarchus 23.7N 47.4W vs mirrored) and latitudes (Mare Serenitatis 28N 17E vs 28S 17E).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoonOrientationSkyTest, "CamSim.Environment.MoonSurface.OrientationOnSky",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMoonOrientationSkyTest::RunTest(const FString&)
{
	// Meeus ex. 53.a geometry: the moon's direction in J2000 and the IAU axes.
	const double D = 2448724.5 - 2451545.0;
	const double Ra = FMath::DegreesToRadians(134.688470), Dec = FMath::DegreesToRadians(13.768368);
	FVector P[3];
	FSkyDomeDriver::PrecessionJ2000ToDate(D / 36525.0, P);
	const FVector3d Date(FMath::Cos(Dec) * FMath::Cos(Ra), FMath::Cos(Dec) * FMath::Sin(Ra), FMath::Sin(Dec));
	const FVector3d Moon(P[0].X * Date.X + P[1].X * Date.Y + P[2].X * Date.Z, P[0].Y * Date.X + P[1].Y * Date.Y + P[2].Y * Date.Z,
		P[0].Z * Date.X + P[1].Z * Date.Y + P[2].Z * Date.Z);
	FVector3d Axes[3];
	FMoonOrientation::IauAxesJ2000(D, Axes);
	const double RaJ = FMath::Atan2(Moon.Y, Moon.X), DecJ = FMath::Asin(Moon.Z);
	const FVector3d East(-FMath::Sin(RaJ), FMath::Cos(RaJ), 0.0);
	const FVector3d North(-FMath::Sin(DecJ) * FMath::Cos(RaJ), -FMath::Sin(DecJ) * FMath::Sin(RaJ), FMath::Cos(DecJ));
	auto Normal = [&Axes](double LatDeg, double LonDeg)
	{
		const double La = FMath::DegreesToRadians(LatDeg), Lo = FMath::DegreesToRadians(LonDeg);
		return Axes[0] * (FMath::Cos(La) * FMath::Cos(Lo)) + Axes[1] * (FMath::Cos(La) * FMath::Sin(Lo)) + Axes[2] * FMath::Sin(La);
	};
	TestTrue(TEXT("Mare Crisium (17N 59E) on the celestial-west side of the disc"), Normal(17.0, 59.0).Dot(East) < -0.3);
	TestTrue(TEXT("Grimaldi (5S 68W) on the celestial-east side"), Normal(-5.2, -68.6).Dot(East) > 0.3);
	TestTrue(TEXT("Mare Frigoris (56N) toward celestial north"), Normal(56.0, 0.0).Dot(North) > 0.5);
	TestTrue(TEXT("both face the observer"), Normal(17.0, 59.0).Dot(-Moon) > 0.0 && Normal(-5.2, -68.6).Dot(-Moon) > 0.0);

	// End to end through the disc model (full moon, uniform lunar-Lambert = 1): the disc point that shows Crisium has
	// Crisium's albedo, the mirrored disc point does not.
	const FMoonAlbedoMap Map = FMoonAlbedoMap::Load(FMoonAlbedoMap::DefaultPath());
	if (Map.IsEmpty())
	{
		AddWarning(TEXT("moon_albedo.pgm not loaded (git lfs pull): map pairs skipped"));
		return true;
	}
	auto A = [&Map](double LatDeg, double LonDeg) { return Map.Sample(FMath::DegreesToRadians(LonDeg), FMath::DegreesToRadians(LatDeg)); };
	TestTrue(TEXT("Humorum dark, its mirrored longitude bright"), A(-24.0, -39.0) < 0.6 * A(-24.0, 39.0));
	TestTrue(TEXT("Smythii dark, its mirrored longitude bright"), A(1.3, 87.0) < 0.6 * A(1.3, -87.0));
	TestTrue(TEXT("Aristarchus bright, its mirrored longitude darker"), A(23.7, -47.4) > 1.5 * A(23.7, 47.4));
	TestTrue(TEXT("Serenitatis dark, its mirrored latitude bright"), A(28.0, 17.0) < 0.6 * A(-28.0, 17.0));
	FMoonDiscFrame F;
	F.MoonDir = Moon;
	F.SunDir = (-Moon + 0.02 * East).GetSafeNormal();   // nearly full, lit from celestial east
	F.Axes[0] = Axes[0]; F.Axes[1] = Axes[1]; F.Axes[2] = Axes[2];
	F.L = 1.0;
	FVector3d Right, Up;
	FMoonDisc::DiscBasis(F, Right, Up);
	const FVector3d Nh = Normal(-24.0, -39.0);   // Humorum
	const double U = Nh.Dot(Right), V = Nh.Dot(Up);
	const double Here = FMoonDisc::Radiance(F, Map, U, V), Mirror = FMoonDisc::Radiance(F, Map, -U, V);
	const double Shade = FMoonDisc::Photometric(Nh.Dot(F.SunDir), FMath::Sqrt(FMath::Max(1.0 - U * U - V * V, 0.0)), 1.0);
	TestTrue(FString::Printf(TEXT("disc point of Humorum shows its albedo (%.3f vs %.3f)"), Here / Shade, A(-24.0, -39.0)),
		FMath::IsNearlyEqual(Here / Shade, A(-24.0, -39.0), 0.01));
	TestTrue(TEXT("the disc's mirrored point is brighter (a flipped map would swap them)"), Mirror > 1.3 * Here);
	return true;
}
