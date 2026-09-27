// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Sensor/SensorController.h"

namespace
{
	/** Every pixel at one log2 level (bin centre). */
	FSensorHistogram Flat(float Log2, uint32 Serial, uint32 Count = 10000)
	{
		FSensorHistogram H;
		H.Bins[FSensorHistogram::BinOf(FMath::Exp2(Log2))] = Count;
		H.Serial = Serial;
		return H;
	}

	FSensorModeConfig EoCfg()
	{
		FSensorModeConfig C;
		C.Exposure.MinGainEv = -20.0f;
		C.Exposure.MaxGainEv = -6.0f;
		C.Exposure.TargetGrey = 0.18f;
		C.Exposure.LagFrames = 0;
		return C;
	}

	FSensorControllerInput In(const FSensorHistogram* H, uint32 Serial, double Dt = 1.0 / 30.0)
	{
		FSensorControllerInput I;
		I.NewHistogram = H;
		I.Serial = Serial;
		I.DeltaSimSec = Dt;
		return I;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorAeMedianTest, "CamSim.Sensor.Controller.MedianToMidGrey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorAeMedianTest::RunTest(const FString& Parameters)
{
	FSensorController C;
	const float SceneLog2 = FSensorHistogram::BinCentreLog2(FSensorHistogram::BinOf(FMath::Exp2(12.0f)));
	const FSensorHistogram H = Flat(12.0f, 1);
	const FSensorFrameParams P = C.Update(In(&H, 1), EoCfg());
	TestEqual(TEXT("median exposed to 0.18"), FMath::Exp2(SceneLog2) * P.Gain, 0.18f, 0.18f * 0.01f);
	TestEqual(TEXT("offset"), P.Offset, 0.0f);
	TestEqual(TEXT("median reported"), C.GetLastMedianLog2(), SceneLog2, 1e-4f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorAeNightTest, "CamSim.Sensor.Controller.NightClampsAtMaxGain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorAeNightTest::RunTest(const FString& Parameters)
{
	FSensorController C;
	const FSensorHistogram H = Flat(-8.0f, 1);  // ~0.004: night
	const FSensorFrameParams P = C.Update(In(&H, 1), EoCfg());
	TestEqual(TEXT("gain at the camera limit"), C.GetGainEv(), -6.0f, 1e-4f);
	TestTrue(TEXT("scene stays dark (< 1% of full scale)"), FMath::Exp2(-8.0f) * P.Gain < 0.01f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorAeSaturatedTest, "CamSim.Sensor.Controller.SaturatedHistogramHitsMinGain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorAeSaturatedTest::RunTest(const FString& Parameters)
{
	// Everything at/above 2^16 wants gain ~2^-18.4; a camera whose shortest
	// integration only reaches 2^-16 must stop there.
	FSensorController C;
	FSensorHistogram H;
	H.Bins[FSensorHistogram::NumBins - 1] = 1000;
	H.Serial = 1;
	FSensorModeConfig Cfg = EoCfg();
	Cfg.Exposure.MinGainEv = -16.0f;
	C.Update(In(&H, 1), Cfg);
	TestEqual(TEXT("gain at the lower limit"), C.GetGainEv(), -16.0f, 1e-4f);
	TestTrue(TEXT("finite"), FMath::IsFinite(C.GetGainEv()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorAeHighlightTest, "CamSim.Sensor.Controller.HighlightCapPreventsClipping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorAeHighlightTest::RunTest(const FString& Parameters)
{
	// 90% at 2^8, 10% at 2^14: exposing the median to 0.18 would put the
	// bright 10% at 0.18 * 64 = 11.5, far past ClipLinear.
	FSensorController C;
	FSensorHistogram H;
	H.Bins[FSensorHistogram::BinOf(FMath::Exp2(8.0f))]  = 9000;
	H.Bins[FSensorHistogram::BinOf(FMath::Exp2(14.0f))] = 1000;
	H.Serial = 1;
	FSensorModeConfig Cfg = EoCfg();
	Cfg.Exposure.HighlightPercentile = 0.95f;
	const FSensorFrameParams P = C.Update(In(&H, 1), Cfg);
	const float Hi = FMath::Exp2(FSensorHistogram::BinCentreLog2(FSensorHistogram::BinOf(FMath::Exp2(14.0f))));
	TestTrue(TEXT("95th percentile not clipped"), Hi * P.Gain <= FSensorController::ClipLinear * 1.0001f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorAeLagTest, "CamSim.Sensor.Controller.LagIsFrameRateIndependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorAeLagTest::RunTest(const FString& Parameters)
{
	// tau = 3 frames at 30 Hz = 0.1 s. After 0.1 s of sim time, 63% of a step.
	for (const double Hz : { 30.0, 60.0 })
	{
		FSensorController C;
		FSensorModeConfig Cfg = EoCfg();
		Cfg.Exposure.LagFrames = 3;
		const FSensorHistogram A = Flat(12.0f, 1);
		C.Update(In(&A, 1), Cfg);                 // first frame snaps
		const float Start = C.GetGainEv();
		const FSensorHistogram B = Flat(10.0f, 2); // 2 stops darker: target +2 EV
		const int32 Steps = FMath::RoundToInt32(0.1 * Hz);
		for (int32 I = 0; I < Steps; ++I)
		{
			const FSensorHistogram Bi = Flat(10.0f, 2 + I);
			C.Update(In(&Bi, 2 + I, 1.0 / Hz), Cfg);
		}
		const float Frac = (C.GetGainEv() - Start) / 2.0f;
		TestEqual(FString::Printf(TEXT("63%% at tau (%.0f Hz)"), Hz), Frac, 1.0f - FMath::Exp(-1.0f), 0.02f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorAeSnapTest, "CamSim.Sensor.Controller.CutAndModeSwitchSnap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorAeSnapTest::RunTest(const FString& Parameters)
{
	FSensorController C;
	FSensorModeConfig Cfg = EoCfg();
	Cfg.Exposure.LagFrames = 30;  // slow: only a snap reaches the target in one step
	const FSensorHistogram A = Flat(12.0f, 1);
	C.Update(In(&A, 1), Cfg);
	const float Day = C.GetGainEv();

	// Cut at serial 5. A histogram measured before the cut (serial 4) must not snap.
	FSensorControllerInput Cut = In(nullptr, 5);
	Cut.bCameraCut = true;
	C.Update(Cut, Cfg);
	const FSensorHistogram Old = Flat(10.0f, 4);
	C.Update(In(&Old, 6), Cfg);
	TestTrue(TEXT("pre-cut histogram eases"), FMath::Abs(C.GetGainEv() - (Day + 2.0f)) > 1.0f);
	const FSensorHistogram New = Flat(10.0f, 5);
	C.Update(In(&New, 7), Cfg);
	TestEqual(TEXT("post-cut histogram snaps"), C.GetGainEv(), Day + 2.0f, 0.01f);

	// Mode switch behaves like a cut.
	FSensorControllerInput Nvg = In(nullptr, 8);
	Nvg.Mode = ESensorGraphMode::NVG;
	C.Update(Nvg, Cfg);
	const FSensorHistogram N = Flat(8.0f, 8);
	FSensorControllerInput NvgH = In(&N, 9);
	NvgH.Mode = ESensorGraphMode::NVG;
	C.Update(NvgH, Cfg);
	TestEqual(TEXT("mode switch snaps"), C.GetGainEv(), Day + 4.0f, 0.01f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorIrAgcTest, "CamSim.Sensor.Controller.IrPercentileStretch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorIrAgcTest::RunTest(const FString& Parameters)
{
	FSensorController C;
	FSensorModeConfig Cfg = EoCfg();
	Cfg.bAGCEnabled = true;
	Cfg.AGCLowPercentile = 0.01f;
	Cfg.AGCHighPercentile = 0.99f;
	Cfg.AGCLagFrames = 0;
	FSensorHistogram H;
	const int32 LoBin = FSensorHistogram::BinOf(FMath::Exp2(4.0f));
	const int32 HiBin = FSensorHistogram::BinOf(FMath::Exp2(6.0f));
	H.Bins[LoBin] = 500; H.Bins[HiBin] = 500; H.Serial = 1;
	FSensorControllerInput I = In(&H, 1);
	I.Mode = ESensorGraphMode::IR;
	const FSensorFrameParams P = C.Update(I, Cfg);
	const float Lo = FMath::Exp2(FSensorHistogram::BinCentreLog2(LoBin));
	const float Hi = FMath::Exp2(FSensorHistogram::BinCentreLog2(HiBin));
	TestEqual(TEXT("low percentile -> 0"), Lo * P.Gain + P.Offset, 0.0f, 1e-4f);
	TestEqual(TEXT("high percentile -> 1"), Hi * P.Gain + P.Offset, 1.0f, 1e-4f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorManualTest, "CamSim.Sensor.Controller.ManualGain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorManualTest::RunTest(const FString& Parameters)
{
	FSensorController C;
	FSensorModeConfig Cfg = EoCfg();
	Cfg.Exposure.bAuto = false;
	Cfg.Exposure.ManualGainEv = -9.0f;
	const FSensorHistogram H = Flat(12.0f, 1);
	const FSensorFrameParams P = C.Update(In(&H, 1), Cfg);
	TestEqual(TEXT("manual gain"), P.Gain, FMath::Exp2(-9.0f), 1e-6f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorZeroDtTest, "CamSim.Sensor.Controller.ZeroDeltaHolds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorZeroDtTest::RunTest(const FString& Parameters)
{
	FSensorController C;
	FSensorModeConfig Cfg = EoCfg();
	Cfg.Exposure.LagFrames = 3;
	const FSensorHistogram A = Flat(12.0f, 1);
	C.Update(In(&A, 1), Cfg);
	const float Before = C.GetGainEv();
	const FSensorHistogram B = Flat(8.0f, 2);
	C.Update(In(&B, 2, 0.0), Cfg);    // frozen clock
	TestEqual(TEXT("frozen: no change"), C.GetGainEv(), Before, 1e-6f);
	C.Update(In(&B, 3, -1.0), Cfg);   // backwards jump
	TestEqual(TEXT("negative dt: no change"), C.GetGainEv(), Before, 1e-6f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorStaleTest, "CamSim.Sensor.Controller.StaleHistogramHolds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorStaleTest::RunTest(const FString& Parameters)
{
	FSensorController C;
	const FSensorHistogram A = Flat(12.0f, 1);
	const FSensorFrameParams First = C.Update(In(&A, 1), EoCfg());
	FSensorFrameParams Last;
	for (int32 I = 0; I < 25; ++I) Last = C.Update(In(nullptr, 2 + I), EoCfg());
	TestEqual(TEXT("gain held"), Last.Gain, First.Gain);
	TestEqual(TEXT("one stale episode"), C.GetStaleEpisodes(), 1u);
	FSensorHistogram Empty; Empty.Serial = 30;
	C.Update(In(&Empty, 30), EoCfg());
	TestEqual(TEXT("empty histogram keeps gain"), C.GetGainEv(), FMath::Log2(First.Gain), 1e-5f);
	return true;
}
