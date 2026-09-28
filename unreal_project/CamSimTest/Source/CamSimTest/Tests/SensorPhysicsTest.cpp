// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include <cmath>
#include "SensorHash.h"
#include "Sensor/SensorReference.h"
#include "Sensor/SensorOptics.h"
#include "Sensor/SensorPresets.h"
#include "Sensor/SensorTypes.h"

// Statistics of the CPU reference detector (ROADMAP 3B.2 Task 7) and optics (Task 8). Flat fields are large enough
// that each tolerance is several standard errors of its estimator; statistics are in double.
namespace
{
	/** Photon-transfer defaults: 16-bit ADC (quantisation negligible), no defects, no dark signal. */
	FSensorFrameParams PhotonParams(float Electrons)
	{
		FSensorFrameParams P;
		P.DetectorType = 0;
		P.FullWellE = 10000.0f;
		P.PhotonGain = Electrons / P.FullWellE;
		P.AnalogGain = 1.0f;
		P.AdcMax = 65535.0f;
		P.Prnu = 0.01f; P.DsnuE = 5.0f; P.ReadNoiseE = 3.0f; P.DarkE = 0.0f;
		P.HotFraction = 0.0f; P.DeadFraction = 0.0f;
		P.Seed = 1; P.FrameIndex = 1;
		return P;
	}

	TArray<float> Flat(int32 W, int32 H, float V) { TArray<float> A; A.Init(V, W * H); return A; }

	TArray<float> Detect(float Signal, int32 W, int32 H, const FSensorFrameParams& P, uint32 Frame)
	{
		FSensorFrameParams Q = P; Q.FrameIndex = Frame;
		return CamSimSensorRef::DetectImage(Flat(W, H, Signal), W, H, 1, Q);
	}

	double Mean(const TArray<double>& A) { double S = 0; for (double V : A) S += V; return S / A.Num(); }
	double Var(const TArray<double>& A)
	{
		const double M = Mean(A); double S = 0;
		for (double V : A) S += (V - M) * (V - M);
		return S / (A.Num() - 1);
	}
	double Corr(const TArray<double>& A, const TArray<double>& B)
	{
		const double Ma = Mean(A), Mb = Mean(B);
		double Sab = 0, Saa = 0, Sbb = 0;
		for (int32 I = 0; I < A.Num(); ++I) { Sab += (A[I] - Ma) * (B[I] - Mb); Saa += (A[I] - Ma) * (A[I] - Ma); Sbb += (B[I] - Mb) * (B[I] - Mb); }
		return Sab / FMath::Sqrt(Saa * Sbb);
	}
	/** Per-pixel combination of two DN images in double. */
	template <typename F> TArray<double> Combine(const TArray<float>& A, const TArray<float>& B, F Fn)
	{
		TArray<double> R; R.SetNumUninitialized(A.Num());
		for (int32 I = 0; I < A.Num(); ++I) R[I] = Fn((double)A[I], (double)B[I]);
		return R;
	}
	/** Mean over frames [First, First + Count) of a flat field, per pixel. */
	TArray<double> FrameAverage(float Signal, int32 W, int32 H, const FSensorFrameParams& P, uint32 First, uint32 Count)
	{
		TArray<double> Acc; Acc.Init(0.0, W * H);
		for (uint32 F = First; F < First + Count; ++F)
		{
			const TArray<float> D = Detect(Signal, W, H, P, F);
			for (int32 I = 0; I < W * H; ++I) Acc[I] += D[I];
		}
		for (double& V : Acc) V /= Count;
		return Acc;
	}
	bool Within(double Measured, double Expected, double RelTol) { return FMath::Abs(Measured - Expected) <= RelTol * FMath::Abs(Expected); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPhysicsPtcTest, "CamSim.Sensor.Physics.PhotonTransfer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPhysicsPtcTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 256, H = 256;
	// { signal e, dark e }: the last case checks dark current carries shot noise.
	for (const FVector2f Case : { FVector2f(50, 0), FVector2f(200, 0), FVector2f(1000, 0), FVector2f(3000, 0),
		FVector2f(6000, 0), FVector2f(9000, 0), FVector2f(200, 300) })
	{
		const float E = Case.X;
		FSensorFrameParams P = PhotonParams(E);
		P.DarkE = Case.Y;
		const double EPerDn = P.FullWellE / P.AdcMax;
		const TArray<float> D1 = Detect(1.0f, W, H, P, 1), D2 = Detect(1.0f, W, H, P, 2);
		const double Temporal = Var(Combine(D1, D2, [](double A, double B) { return A - B; })) / 2.0 * EPerDn * EPerDn;
		const double Fixed = Var(Combine(D1, D2, [](double A, double B) { return 0.5 * (A + B); })) * EPerDn * EPerDn - Temporal / 2.0;
		const double ExpTemporal = E + P.DarkE + P.ReadNoiseE * P.ReadNoiseE;
		const double ExpFixed = FMath::Square((double)P.Prnu * E) + (double)P.DsnuE * P.DsnuE;
		AddInfo(FString::Printf(TEXT("e=%.0f dark=%.0f: temporal %.2f e^2 (expected %.2f), fixed %.2f e^2 (expected %.2f)"),
			E, P.DarkE, Temporal, ExpTemporal, Fixed, ExpFixed));
		TestTrue(FString::Printf(TEXT("e=%.0f dark=%.0f temporal variance within 5%%"), E, P.DarkE), Within(Temporal, ExpTemporal, 0.05));
		TestTrue(FString::Printf(TEXT("e=%.0f dark=%.0f fixed-pattern variance within 5%%"), E, P.DarkE), Within(Fixed, ExpFixed, 0.05));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPhysicsDeterminismTest, "CamSim.Sensor.Physics.Determinism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPhysicsDeterminismTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 256, H = 256;
	// PRNU 5% at 5000 e: the fixed pattern (~250 e rms) dominates a 16-frame average's residual
	// temporal noise (~18 e rms), so two independent 16-frame averages correlate at ~0.995.
	FSensorFrameParams P = PhotonParams(5000.0f);
	P.Prnu = 0.05f;
	const TArray<float> A = Detect(1.0f, W, H, P, 1), B = Detect(1.0f, W, H, P, 1);
	TestTrue(TEXT("same params + frame -> bit-identical"), FMemory::Memcmp(A.GetData(), B.GetData(), A.Num() * sizeof(float)) == 0);

	// Temporal residuals of frames 1 and 2 (each against an independent frame: 3 and 4).
	const TArray<double> R1 = Combine(Detect(1.0f, W, H, P, 1), Detect(1.0f, W, H, P, 3), [](double X, double Y) { return X - Y; });
	const TArray<double> R2 = Combine(Detect(1.0f, W, H, P, 2), Detect(1.0f, W, H, P, 4), [](double X, double Y) { return X - Y; });
	const double RhoTemporal = Corr(R1, R2);

	const TArray<double> FpA = FrameAverage(1.0f, W, H, P, 1, 16);
	const TArray<double> FpB = FrameAverage(1.0f, W, H, P, 17, 16);
	const double RhoFixed = Corr(FpA, FpB);

	FSensorFrameParams P2 = P; P2.Seed = 2;
	const double RhoSeed = Corr(FpA, FrameAverage(1.0f, W, H, P2, 1, 16));
	AddInfo(FString::Printf(TEXT("temporal rho %.4f, fixed-pattern rho %.4f, seed 1 vs 2 rho %.4f"), RhoTemporal, RhoFixed, RhoSeed));
	TestTrue(TEXT("frames 1 vs 2 temporal residuals uncorrelated"), FMath::Abs(RhoTemporal) < 0.05);
	TestTrue(TEXT("fixed pattern repeats across frame sets"), RhoFixed > 0.95);
	TestTrue(TEXT("seeds 1 vs 2 fixed patterns uncorrelated"), FMath::Abs(RhoSeed) < 0.05);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPhysicsHashTest, "CamSim.Sensor.Physics.HashGaussianStatistics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPhysicsHashTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 256, H = 256;
	TArray<double> G; G.SetNumUninitialized(W * H);
	for (int32 Y = 0; Y < H; ++Y) for (int32 X = 0; X < W; ++X) G[Y * W + X] = CamSimHash::Gaussian(X, Y, 0, 0, 0);
	const double M = Mean(G);
	double M2 = 0, M3 = 0, M4 = 0;
	for (double V : G) { const double D = V - M; M2 += D * D; M3 += D * D * D; M4 += D * D * D * D; }
	M2 /= G.Num(); M3 /= G.Num(); M4 /= G.Num();
	const double Sigma = FMath::Sqrt(M2), Skew = M3 / (M2 * Sigma), Kurt = M4 / (M2 * M2);
	AddInfo(FString::Printf(TEXT("mean %.5f, sigma %.5f, skew %.5f, kurtosis %.5f"), M, Sigma, Skew, Kurt));
	TestTrue(TEXT("|mean| < 0.01"), FMath::Abs(M) < 0.01);
	TestTrue(TEXT("sigma within 1% of 1"), FMath::Abs(Sigma - 1.0) < 0.01);
	TestTrue(TEXT("|skew| < 0.05"), FMath::Abs(Skew) < 0.05);
	TestTrue(TEXT("kurtosis within 0.1 of 3"), FMath::Abs(Kurt - 3.0) < 0.1);
	// Integer-exact PCG (mirrored in HLSL): pin one value so a port can be checked against it.
	TestEqual(TEXT("Pcg(0)"), CamSimHash::Pcg(0u), 129708002u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPhysicsBolometerTest, "CamSim.Sensor.Physics.MicrobolometerFpn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPhysicsBolometerTest::RunTest(const FString& Parameters)
{
	// 1024 columns/rows: the std of 1024 column means is within ~2.2% (1 sigma) of the true FPN.
	constexpr int32 W = 1024, H = 1024;
	FSensorFrameParams P;
	P.DetectorType = 1;
	P.PhotonGain = 1.0f; P.AdcMax = 65535.0f;
	P.TemporalNoise = 0.0f; P.PixelFpn = 0.0f; P.ColumnFpn = 0.002f; P.RowFpn = 0.001f;
	P.HotFraction = 0.0f; P.DeadFraction = 0.0f;
	P.FrameIndex = 1;
	auto LineStds = [&](const TArray<float>& D, double& ColStd, double& RowStd, double& ResidStd)
	{
		TArray<double> Cols; Cols.Init(0.0, W);
		TArray<double> Rows; Rows.Init(0.0, H);
		TArray<double> All; All.SetNumUninitialized(W * H);
		for (int32 Y = 0; Y < H; ++Y) for (int32 X = 0; X < W; ++X) { Cols[X] += D[Y * W + X] / H; Rows[Y] += D[Y * W + X] / W; All[Y * W + X] = D[Y * W + X]; }
		ColStd = FMath::Sqrt(Var(Cols)) / P.AdcMax;
		RowStd = FMath::Sqrt(Var(Rows)) / P.AdcMax;
		ResidStd = FMath::Sqrt(Var(All)) / P.AdcMax;
	};
	double ColStd, RowStd, Resid05, Resid02;
	LineStds(CamSimSensorRef::DetectImage(Flat(W, H, 0.5f), W, H, 1, P), ColStd, RowStd, Resid05);
	double C2, R2;
	LineStds(CamSimSensorRef::DetectImage(Flat(W, H, 0.2f), W, H, 1, P), C2, R2, Resid02);
	AddInfo(FString::Printf(TEXT("column std %.6f (0.002), row std %.6f (0.001), residual std %.6f at 0.5 vs %.6f at 0.2"),
		ColStd, RowStd, Resid05, Resid02));
	TestTrue(TEXT("column FPN within 10%"), Within(ColStd, 0.002, 0.10));
	TestTrue(TEXT("row FPN within 10%"), Within(RowStd, 0.001, 0.10));
	TestTrue(TEXT("FPN independent of signal (2%)"), Within(Resid02, Resid05, 0.02));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPhysicsDefectTest, "CamSim.Sensor.Physics.DefectFractions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPhysicsDefectTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 512, H = 512;
	FSensorFrameParams P = PhotonParams(5000.0f);   // mid-scale: noise never reaches 0 or AdcMax
	P.HotFraction = 1e-3f; P.DeadFraction = 1e-3f;
	const TArray<float> D = Detect(1.0f, W, H, P, 1);
	int32 Hot = 0, Dead = 0;
	for (float V : D) { Hot += V == P.AdcMax; Dead += V == 0.0f; }
	const double Expected = W * H * 1e-3, FourSigma = 4.0 * FMath::Sqrt(Expected * (1.0 - 1e-3));
	AddInfo(FString::Printf(TEXT("hot %d, dead %d (expected %.0f +- %.0f)"), Hot, Dead, Expected, FourSigma));
	TestTrue(TEXT("hot count within 4 sigma"), FMath::Abs(Hot - Expected) <= FourSigma);
	TestTrue(TEXT("dead count within 4 sigma"), FMath::Abs(Dead - Expected) <= FourSigma);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPhysicsClipTest, "CamSim.Sensor.Physics.ClipsAtFullWellAndZero",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPhysicsClipTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 256, H = 256;
	FSensorFrameParams P = PhotonParams(30000.0f);   // Signal * PhotonGain = 3: three full wells
	P.HotFraction = 1e-3f; P.DeadFraction = 1e-3f;
	const TArray<float> D = Detect(1.0f, W, H, P, 1);
	int32 Bad = 0, Dead = 0;
	for (int32 Y = 0; Y < H; ++Y)
	{
		for (int32 X = 0; X < W; ++X)
		{
			const bool bDead = CamSimHash::Uniform(CamSimHash::Hash(X, Y, CamSimHash::FixedFrame, P.Seed, 2 * 9)) > 1.0f - P.DeadFraction;
			Dead += bDead;
			Bad += bDead ? D[Y * W + X] != 0.0f : D[Y * W + X] != P.AdcMax;
		}
	}
	AddInfo(FString::Printf(TEXT("3x full well: %d dead pixels at 0, %d others not at AdcMax"), Dead, Bad));
	TestEqual(TEXT("saturated except dead pixels"), Bad, 0);

	FSensorFrameParams Z = PhotonParams(0.0f);
	Z.DsnuE = 0.0f; Z.DarkE = 0.0f; Z.ReadNoiseE = 2.0f;
	const TArray<float> D0 = Detect(0.0f, W, H, Z, 1);
	int32 Negative = 0, NaNs = 0; double Sum = 0;
	for (float V : D0) { Negative += V < 0.0f; NaNs += FMath::IsNaN(V); Sum += V; }
	AddInfo(FString::Printf(TEXT("dark frame: mean %.3f DN, %d negative, %d NaN"), Sum / D0.Num(), Negative, NaNs));
	TestEqual(TEXT("no negative DN"), Negative, 0);
	TestEqual(TEXT("no NaN"), NaNs, 0);
	TestTrue(TEXT("mean >= 0"), Sum >= 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPhysicsAnalogGainTest, "CamSim.Sensor.Physics.AnalogGainAmplifiesNoise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPhysicsAnalogGainTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 256, H = 256;
	auto Measure = [&](float AnalogGain, double& MeanDn, double& Snr)
	{
		FSensorFrameParams P = PhotonParams(20.0f);
		P.DsnuE = 0.0f; P.AnalogGain = AnalogGain;
		const TArray<float> D1 = Detect(1.0f, W, H, P, 1), D2 = Detect(1.0f, W, H, P, 2);
		MeanDn = Mean(Combine(D1, D2, [](double A, double B) { return 0.5 * (A + B); }));
		Snr = MeanDn / FMath::Sqrt(Var(Combine(D1, D2, [](double A, double B) { return A - B; })) / 2.0);
	};
	double M1, S1, M8, S8;
	Measure(1.0f, M1, S1);
	Measure(8.0f, M8, S8);
	AddInfo(FString::Printf(TEXT("gain 1: mean %.2f DN, SNR %.3f; gain 8: mean %.2f DN, SNR %.3f"), M1, S1, M8, S8));
	TestTrue(TEXT("temporal SNR unchanged by analog gain (10%)"), Within(S8, S1, 0.10));
	TestTrue(TEXT("mean DN scales 8x (2%)"), Within(M8, 8.0 * M1, 0.02));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPhysicsNaNGuardTest, "CamSim.Sensor.Physics.NegativeElectronsStayFinite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPhysicsNaNGuardTest::RunTest(const FString& Parameters)
{
	// PRNU 0.5 at 1000 e: ~2% of pixels have e1 < 0 (1 + 0.5 G < 0 for G < -2). The shot-noise sqrt
	// and every clamp must still see finite values (FMath::Clamp(NaN) differs from HLSL clamp).
	constexpr int32 W = 256, H = 256;
	FSensorFrameParams P = PhotonParams(1000.0f);
	P.Prnu = 0.5f;
	const TArray<float> D = Detect(1.0f, W, H, P, 1);
	int32 Bad = 0, Zero = 0;
	for (float V : D) { Bad += !FMath::IsFinite(V) || V < 0.0f || V > P.AdcMax; Zero += V == 0.0f; }
	AddInfo(FString::Printf(TEXT("Prnu 0.5: %d of %d pixels clipped to 0, %d non-finite or out of range"), Zero, W * H, Bad));
	TestEqual(TEXT("every DN finite and within [0, AdcMax]"), Bad, 0);
	TestTrue(TEXT("negative e1 pixels exist (~2%)"), Zero > W * H / 100);

	// A NaN signal is treated as 0 for both detector types.
	for (uint32 Type : { 0u, 1u })
	{
		FSensorFrameParams Q = P; Q.DetectorType = Type;
		const float Dn = CamSimSensorRef::DetectPixel(NAN, 3, 5, 0, Q);
		TestTrue(FString::Printf(TEXT("type %u: NaN signal -> finite DN (%f)"), Type, Dn), FMath::IsFinite(Dn) && Dn >= 0.0f && Dn <= Q.AdcMax);
		TestEqual(FString::Printf(TEXT("type %u: NaN signal reads as 0"), Type), Dn, CamSimSensorRef::DetectPixel(0.0f, 3, 5, 0, Q));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Optics (ROADMAP 3B.2 Task 8)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPhysicsDistortionTest, "CamSim.Sensor.Physics.DistortionMatchesForwardModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPhysicsDistortionTest::RunTest(const FString& Parameters)
{
	// Brown-Conrady forward model rd = ru (1 + K1 ru^2 + K2 ru^4); UndistortRadius must invert it
	// over the whole 1280x720 / 60 deg frame (9x9 grid of ideal points, corners included).
	constexpr int32 W = 1280, H = 720;
	constexpr float K1 = -0.2f, K2 = 0.05f;
	const float F = CamSimOptics::FocalPx(W, 60.0f);
	double Worst = 0.0;
	int32 Failed = 0;
	for (int32 J = 0; J < 9; ++J)
	{
		for (int32 I = 0; I < 9; ++I)
		{
			const float X = (-0.5f * W + W * I / 8.0f) / F, Y = (-0.5f * H + H * J / 8.0f) / F;
			const float Ru = FMath::Sqrt(X * X + Y * Y);
			const float Rd = Ru * (1.0f + K1 * Ru * Ru + K2 * Ru * Ru * Ru * Ru);
			float Out = -1.0f;
			Failed += !CamSimOptics::UndistortRadius(Rd, K1, K2, Out);
			Worst = FMath::Max(Worst, (double)FMath::Abs(Out - Ru));
		}
	}
	AddInfo(FString::Printf(TEXT("FocalPx %.3f; worst |ru - recovered| = %.3g normalised = %.3g px"), F, Worst, Worst * F));
	TestEqual(TEXT("every grid point converges"), Failed, 0);
	TestTrue(TEXT("recovered within 1e-4 normalised"), Worst < 1e-4);
	TestTrue(TEXT("recovered within 0.1 px"), Worst * F < 0.1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPhysicsNewtonTest, "CamSim.Sensor.Physics.NewtonConvergesOrConfigRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPhysicsNewtonTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 1280, H = 720;
	const float F = CamSimOptics::FocalPx(W, 60.0f);
	const float Corner = CamSimOptics::CornerRadius(W, H, F);
	TestTrue(TEXT("corner radius = hypot(W/2, H/2) / FocalPx"), FMath::IsNearlyEqual(Corner, FMath::Sqrt(640.0f * 640.0f + 360.0f * 360.0f) / F, 1e-5f));

	// K1 = -0.3: rd(ru) peaks at 0.703 > corner 0.662, so a root exists and 3 iterations reach it.
	float Ru = 0.0f;
	TestTrue(TEXT("K1 -0.3 converges at the corner"), CamSimOptics::UndistortRadius(Corner, -0.3f, 0.0f, Ru));
	AddInfo(FString::Printf(TEXT("K1 -0.3: corner rd %.5f -> ru %.5f, forward %.7f"), Corner, Ru, Ru * (1.0f - 0.3f * Ru * Ru)));
	TestTrue(TEXT("K1 -0.3 root satisfies the forward model"), FMath::Abs(Ru * (1.0f - 0.3f * Ru * Ru) - Corner) <= 1e-5f);

	// K1 = -1.0: rd(ru) peaks at 0.385 < corner: no ideal radius maps to the corner -> rejected.
	TestFalse(TEXT("K1 -1.0 is rejected"), CamSimOptics::UndistortRadius(Corner, -1.0f, 0.0f, Ru));

	// rd = 0 is the optical axis: ru = 0.
	TestTrue(TEXT("rd 0 converges"), CamSimOptics::UndistortRadius(0.0f, -0.3f, 0.0f, Ru) && Ru == 0.0f);
	return true;
}

namespace
{
	/** Optics params: FocalPx from HFOV, no distortion, no blur, noiseless-irrelevant. */
	FSensorFrameParams OpticsParams(int32 W, float HFovDeg)
	{
		FSensorFrameParams P;
		P.FocalPx = CamSimOptics::FocalPx(W, HFovDeg);
		P.K1 = 0.0f; P.K2 = 0.0f; P.VignettingExponent = 0.0f; P.PsfSigmaPx = 0.0f;
		return P;
	}
	TArray<FLinearColor> SolidScene(int32 W, int32 H, FLinearColor C) { TArray<FLinearColor> A; A.Init(C, W * H); return A; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPhysicsVignettingTest, "CamSim.Sensor.Physics.VignettingIsCosN",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPhysicsVignettingTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 1280, H = 720;
	FSensorFrameParams P = OpticsParams(W, 60.0f);
	P.VignettingExponent = 4.0f;
	TArray<FVector3f> Rgb;
	FSensorHistogram Hist;
	CamSimSensorRef::Optics(SolidScene(W, H, FLinearColor(1, 1, 1)), W, H, W, H, P, Rgb, Hist);
	TestEqual(TEXT("output size"), Rgb.Num(), W * H);
	TestEqual(TEXT("histogram counts every pixel"), Hist.Total(), (uint64)(W * H));

	// Expected: cos^4 of the ray angle through the pixel centre.
	auto Expected = [&](int32 X, int32 Y)
	{
		const double Xd = (X + 0.5 - W / 2.0) / P.FocalPx, Yd = (Y + 0.5 - H / 2.0) / P.FocalPx;
		return FMath::Pow(1.0 / FMath::Sqrt(1.0 + Xd * Xd + Yd * Yd), 4.0);
	};
	const FIntPoint Probes[] = { {0, 0}, {W - 1, 0}, {0, H - 1}, {W - 1, H - 1},   // corners
		{W / 2, 0}, {W / 2, H - 1}, {0, H / 2}, {W - 1, H / 2}, {W / 2, H / 2} };   // edge midpoints, centre
	for (const FIntPoint& Q : Probes)
	{
		const double Got = Rgb[Q.Y * W + Q.X].X, Want = Expected(Q.X, Q.Y);
		AddInfo(FString::Printf(TEXT("(%d,%d): %.5f vs cos^4 %.5f (%.3f%%)"), Q.X, Q.Y, Got, Want, 100.0 * (Got - Want) / Want));
		TestTrue(FString::Printf(TEXT("(%d,%d) within 1%% of cos^4"), Q.X, Q.Y), Within(Got, Want, 0.01));
		TestTrue(FString::Printf(TEXT("(%d,%d) grey"), Q.X, Q.Y), Rgb[Q.Y * W + Q.X].X == Rgb[Q.Y * W + Q.X].Y && Rgb[Q.Y * W + Q.X].Y == Rgb[Q.Y * W + Q.X].Z);
	}
	// Corner of a 60 deg HFOV 16:9 frame: cos^4 ~ 0.48 — clearly visible vignetting.
	TestTrue(TEXT("corner darker than 0.6"), Rgb[0].X < 0.6f);

	// FocalPx 0 = optics off: identity, no illumination falloff.
	FSensorFrameParams Off = P; Off.FocalPx = 0.0f;
	CamSimSensorRef::Optics(SolidScene(W, H, FLinearColor(1, 1, 1)), W, H, W, H, Off, Rgb, Hist);
	TestEqual(TEXT("optics off: corner untouched"), Rgb[0].X, 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPhysicsPsfEdgeTest, "CamSim.Sensor.Physics.PsfEdgeSpread",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPhysicsPsfEdgeTest::RunTest(const FString& Parameters)
{
	// Vertical step edge 0 | 1 between columns W/2-1 and W/2, blurred with sigma 1.3 px. Fit the
	// edge spread function 0.5 (1 + erf((x - e) / (sigma sqrt 2))) over (e, sigma) by least squares.
	// Expected fit ~2.6% low: the taps are a point-sampled Gaussian (kernel std 1.296 after the 3.08
	// sigma truncation), while a continuous erf sampled at pixel centres models a pixel-integrated
	// LSF, which carries an extra 1/12 px^2 of variance: sigma_fit^2 ~ 1.296^2 - 1/12 -> 1.264.
	constexpr int32 W = 64, H = 8;
	constexpr float Sigma = 1.3f;
	TArray<FVector3f> Img;
	Img.SetNumUninitialized(W * H);
	for (int32 Y = 0; Y < H; ++Y) for (int32 X = 0; X < W; ++X) Img[Y * W + X] = FVector3f(X < W / 2 ? 0.0f : 1.0f);
	CamSimSensorRef::Blur(Img, W, H, Sigma);

	const int32 Row = H / 2;
	auto Sse = [&](double E, double S)
	{
		double Sum = 0.0;
		for (int32 X = 0; X < W; ++X)
		{
			const double Model = 0.5 * (1.0 + std::erf((X - E) / (S * UE_DOUBLE_SQRT_2)));
			const double D = Img[Row * W + X].X - Model;
			Sum += D * D;
		}
		return Sum;
	};
	// Coarse grid, then two refinements around the best point.
	double BestE = W / 2.0, BestS = 1.0, Best = Sse(BestE, BestS);
	double StepE = 0.05, StepS = 0.02, SpanE = 2.0, SpanS = 1.5, CentreE = W / 2.0, CentreS = 1.75;
	for (int32 Pass = 0; Pass < 3; ++Pass)
	{
		for (double E = CentreE - SpanE; E <= CentreE + SpanE; E += StepE)
			for (double S = FMath::Max(0.05, CentreS - SpanS); S <= CentreS + SpanS; S += StepS)
			{
				const double V = Sse(E, S);
				if (V < Best) { Best = V; BestE = E; BestS = S; }
			}
		CentreE = BestE; CentreS = BestS; SpanE = StepE * 2; SpanS = StepS * 2; StepE /= 10; StepS /= 10;
	}
	AddInfo(FString::Printf(TEXT("ESF fit: sigma %.4f px (target %.2f, %.2f%%), edge at %.3f, rms resid %.2g"),
		BestS, Sigma, 100.0 * (BestS - Sigma) / Sigma, BestE, FMath::Sqrt(Best / W)));
	TestTrue(TEXT("fitted sigma within 5% of 1.3 px"), Within(BestS, Sigma, 0.05));

	// Every row is the same (vertical pass over a column-constant image changes nothing) and the
	// far field keeps its value (clamp-to-edge, normalised taps).
	TestTrue(TEXT("left edge stays 0"), FMath::Abs(Img[Row * W].X) < 1e-6f);
	TestTrue(TEXT("right edge stays 1"), FMath::Abs(Img[Row * W + W - 1].X - 1.0f) < 1e-6f);
	TestTrue(TEXT("rows identical"), Img[0 * W + W / 2].X == Img[(H - 1) * W + W / 2].X);

	// Taps: radius min(ceil(3 sigma), 8), centre first, normalised over the symmetric kernel.
	TArray<float> Taps;
	CamSimOptics::PsfTaps(Sigma, Taps);
	TestEqual(TEXT("sigma 1.3: radius 4 -> 5 taps"), Taps.Num(), 5);
	double Total = Taps[0];
	for (int32 K = 1; K < Taps.Num(); ++K) Total += 2.0 * Taps[K];
	TestTrue(TEXT("taps normalised"), FMath::Abs(Total - 1.0) < 1e-6);
	CamSimOptics::PsfTaps(5.0f, Taps);
	TestEqual(TEXT("sigma 5: radius capped at 8 -> 9 taps"), Taps.Num(), 9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPhysicsZoomTest, "CamSim.Sensor.Physics.OpticsAcrossZoomRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPhysicsZoomTest::RunTest(const FString& Parameters)
{
	constexpr int32 W = 1920, H = 1080;
	FSensorOpticsConfig O;   // struct defaults
	float Sigma[2];
	int32 I = 0;
	for (const float HFov : { 1.0f, 60.0f })
	{
		const float F = CamSimOptics::FocalPx(W, HFov);
		const float R = CamSimOptics::CornerRadius(W, H, F);
		const float Cos4 = FMath::Pow(1.0f / FMath::Sqrt(1.0f + R * R), 4.0f);
		Sigma[I++] = CamSimOptics::PsfSigmaPx(O);
		AddInfo(FString::Printf(TEXT("HFOV %.0f deg: FocalPx %.1f, corner r %.5f, cos^4 %.5f, PSF sigma %.4f px"), HFov, F, R, Cos4, Sigma[I - 1]));
		TestTrue(FString::Printf(TEXT("HFOV %.0f: FocalPx finite and > 0"), HFov), FMath::IsFinite(F) && F > 0.0f);
		TestTrue(FString::Printf(TEXT("HFOV %.0f: corner cos^4 in (0, 1]"), HFov), Cos4 > 0.0f && Cos4 <= 1.0f);
	}
	TestEqual(TEXT("PSF sigma independent of FOV"), Sigma[0], Sigma[1]);
	TestTrue(TEXT("60 deg is wider than 1 deg (shorter focal length)"), CamSimOptics::FocalPx(W, 60.0f) < CamSimOptics::FocalPx(W, 1.0f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorPhysicsPsfDatasheetTest, "CamSim.Sensor.Physics.PsfSigmaFromDatasheet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorPhysicsPsfDatasheetTest::RunTest(const FString& Parameters)
{
	// sigma = sqrt((0.42 lambda N / pitch)^2 + 0.29^2 + extra^2): Airy core as a Gaussian plus the
	// pixel aperture (1/sqrt(12) px) — evaluated in double as the expected value.
	struct FCase { const TCHAR* Preset; double Lambda, N, Pitch; };
	for (const FCase& C : { FCase{ TEXT("eo_hd_cmos"), 0.55, 4.0, 2.9 }, FCase{ TEXT("mwir_cooled"), 4.0, 4.0, 15.0 } })
	{
		FSensorModeConfig M;
		TestTrue(FString::Printf(TEXT("%s preset exists"), C.Preset), CamSimSensorPresets::Apply(C.Preset, M));
		const double Airy = 0.42 * C.Lambda * C.N / C.Pitch;
		const double Want = FMath::Sqrt(Airy * Airy + 0.29 * 0.29);
		const float Got = CamSimOptics::PsfSigmaPx(M.Optics);
		AddInfo(FString::Printf(TEXT("%s: sigma %.5f px (expected %.5f)"), C.Preset, Got, Want));
		TestTrue(FString::Printf(TEXT("%s: sigma within 0.001 px"), C.Preset), FMath::Abs(Got - Want) <= 0.001);
	}
	FSensorOpticsConfig Extra;
	Extra.WavelengthUm = 0.0f; Extra.ExtraBlurPx = 1.0f;
	TestTrue(TEXT("extra blur adds in quadrature"), FMath::IsNearlyEqual(CamSimOptics::PsfSigmaPx(Extra), FMath::Sqrt(0.29f * 0.29f + 1.0f), 1e-5f));
	return true;
}
