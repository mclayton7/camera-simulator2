// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "SensorHash.h"
#include "Sensor/SensorReference.h"

// Statistics of the CPU reference detector (ROADMAP 3B.2 Task 7). Flat fields are large enough
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
