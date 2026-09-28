// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Sensor/SensorReference.h"

namespace
{
	TArray<FLinearColor> Solid(int32 W, int32 H, FLinearColor C) { TArray<FLinearColor> A; A.Init(C, W * H); return A; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorRefEoGreyTest, "CamSim.Sensor.Reference.EoGreyLimitedRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorRefEoGreyTest::RunTest(const FString& Parameters)
{
	FSensorFrameParams P;  // EO, gain 1
	const auto R0 = CamSimSensorRef::Run(Solid(8, 4, FLinearColor(0, 0, 0)), 8, 4, P);
	TestEqual(TEXT("black -> Y 16"), (int32)R0.Nv12[0], 16);
	TestEqual(TEXT("black chroma 128"), (int32)R0.Nv12[8 * 4], 128);
	const auto R1 = CamSimSensorRef::Run(Solid(8, 4, FLinearColor(100, 100, 100)), 8, 4, P);
	TestEqual(TEXT("far over knee -> Y 235"), (int32)R1.Nv12[0], 235);
	const float Grey = 0.18f;
	const auto R2 = CamSimSensorRef::Run(Solid(8, 4, FLinearColor(Grey, Grey, Grey)), 8, 4, P);
	const int32 Expected = FMath::RoundToInt32(16.0f + 219.0f * CamSimSensorRef::Oetf709(Grey));
	TestEqual(TEXT("0.18 through BT.709 OETF"), (int32)R2.Nv12[0], Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorRefEoAnalogGainTest, "CamSim.Sensor.Reference.EoGainIsPhotonTimesAnalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorRefEoAnalogGainTest::RunTest(const FString& Parameters)
{
	// Until the detector model lands, EO displays signal * PhotonGain * AnalogGain.
	FSensorFrameParams A; A.PhotonGain = 0.25f;
	FSensorFrameParams B; B.PhotonGain = 0.125f; B.AnalogGain = 2.0f;
	const auto Scene = Solid(8, 4, FLinearColor(0.7f, 1.3f, 2.1f));
	TestTrue(TEXT("same image"), CamSimSensorRef::Run(Scene, 8, 4, B).Nv12 == CamSimSensorRef::Run(Scene, 8, 4, A).Nv12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorRefHistogramTest, "CamSim.Sensor.Reference.HistogramCountsEveryPixel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorRefHistogramTest::RunTest(const FString& Parameters)
{
	// 1.03, not 1.0: the BT.709 weights may sum to 1 - 1 ulp, which would put 1.0 in bin 127.
	TArray<FLinearColor> Scene = Solid(8, 4, FLinearColor(1.03f, 1.03f, 1.03f));
	Scene[0] = FLinearColor(NAN, 0, 0);
	Scene[1] = FLinearColor(-5, -5, -5);
	Scene[2] = FLinearColor(INFINITY, INFINITY, INFINITY);
	FSensorFrameParams P;
	const auto R = CamSimSensorRef::Run(Scene, 8, 4, P);
	TestEqual(TEXT("total"), R.Histogram.Total(), (uint64)32);
	TestEqual(TEXT("NaN and negative in bin 0"), R.Histogram.Bins[0], 2u);
	TestEqual(TEXT("Inf clamps to 65504 -> bin of 65504"), R.Histogram.Bins[FSensorHistogram::BinOf(65504.0f)], 1u);
	TestEqual(TEXT("1.03 -> bin 128"), R.Histogram.Bins[128], 29u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorRefIrTest, "CamSim.Sensor.Reference.IrGainOffsetPolarity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorRefIrTest::RunTest(const FString& Parameters)
{
	FSensorFrameParams P;
	P.Mode = ESensorGraphMode::IR;
	P.PhotonGain = 0.5f; P.DisplayOffset = 0.25f;   // s = 1 -> 0.75
	const auto White = CamSimSensorRef::Run(Solid(8, 4, FLinearColor(1, 1, 1)), 8, 4, P);
	TestEqual(TEXT("white-hot"), (int32)White.Nv12[0], FMath::RoundToInt32(16 + 219 * 0.75f));
	FSensorFrameParams Agc = P;
	Agc.PhotonGain = 0.125f; Agc.DisplayGain = 4.0f;  // AGC on normalised DN: 4 * (1 * 0.125) + 0.25 = 0.75
	const auto AgcWhite = CamSimSensorRef::Run(Solid(8, 4, FLinearColor(1, 1, 1)), 8, 4, Agc);
	TestEqual(TEXT("display gain on normalised signal"), (int32)AgcWhite.Nv12[0], FMath::RoundToInt32(16 + 219 * 0.75f));
	TestEqual(TEXT("IR chroma neutral"), (int32)White.Nv12[8 * 4 + 1], 128);
	P.bBlackHot = 1;
	const auto Black = CamSimSensorRef::Run(Solid(8, 4, FLinearColor(1, 1, 1)), 8, 4, P);
	TestEqual(TEXT("black-hot"), (int32)Black.Nv12[0], FMath::RoundToInt32(16 + 219 * 0.25f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSensorRefChromaTest, "CamSim.Sensor.Reference.EoChromaFrom2x2Mean",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSensorRefChromaTest::RunTest(const FString& Parameters)
{
	FSensorFrameParams P;
	P.KneeStart = 1.0f;  // no knee: exact OETF
	const auto R = CamSimSensorRef::Run(Solid(4, 2, FLinearColor(0.5f, 0.0f, 0.0f)), 4, 2, P);
	const float Rp = CamSimSensorRef::Oetf709(0.5f);
	const float Y = 0.2126f * Rp;
	TestEqual(TEXT("Y"), (int32)R.Nv12[0], FMath::RoundToInt32(16 + 219 * Y));
	TestEqual(TEXT("Cb"), (int32)R.Nv12[8], FMath::RoundToInt32(128 + 224 * (0.0f - Y) / 1.8556f));
	TestEqual(TEXT("Cr"), (int32)R.Nv12[9], FMath::RoundToInt32(128 + 224 * (Rp - Y) / 1.5748f));
	return true;
}
