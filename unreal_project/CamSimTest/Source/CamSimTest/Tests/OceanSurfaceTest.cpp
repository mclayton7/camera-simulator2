// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Ocean/OceanSurface.h"

#include <limits>

namespace
{
	FOceanSurface MakeSurface(double Geoid = -32.0)
	{
		return FOceanSurface([Geoid](double, double) { return TOptional<double>(Geoid); });
	}
	FOceanWave HostWave(double H, double L, double From)
	{
		FOceanWave W; W.HeightM = H; W.LengthM = L; W.FromDeg = From; return W;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanSurfaceSeaLevelTest, "CamSim.Ocean.Surface.SeaLevelIsGeoidPlusTide",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanSurfaceSeaLevelTest::RunTest(const FString& Parameters)
{
	FOceanSurface S = MakeSurface(-32.0);
	S.SetAnchor(37.8, -122.4);
	TestEqual(TEXT("geoid"), S.SeaLevelM(37.8, -122.4).Get(0.0), -32.0, 1e-12);
	S.SetTideOffsetM(1.5);
	TestEqual(TEXT("geoid + tide"), S.SeaLevelM(37.8, -122.4).Get(0.0), -30.5, 1e-12);
	TestEqual(TEXT("calm: surface = sea level"), S.SurfaceHeightM(37.8, -122.4).Get(0.0), -30.5, 1e-12);

	FOceanSurface NoGrid([](double, double) { return TOptional<double>(); });
	NoGrid.SetAnchor(37.8, -122.4);
	TestFalse(TEXT("no geoid → no sea level"), NoGrid.SeaLevelM(37.8, -122.4).IsSet());
	TestFalse(TEXT("no geoid → no surface"), NoGrid.SurfaceHeightM(37.8, -122.4).IsSet());
	TestFalse(TEXT("no geoid → no normal"), NoGrid.SurfaceNormalNeu(37.8, -122.4).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanSurfaceDatumOffsetTest, "CamSim.Ocean.Surface.DatumOffsetAddsUnderTide",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanSurfaceDatumOffsetTest::RunTest(const FString& Parameters)
{
	FOceanSurface S = MakeSurface(-35.0);
	S.SetAnchor(33.2, -117.4);
	S.SetDatumOffsetM(0.39);
	TestEqual(TEXT("geoid + datum"), S.SeaLevelM(33.2, -117.4).Get(0.0), -34.61, 1e-12);
	S.SetTideOffsetM(-0.5);
	TestEqual(TEXT("geoid + datum + tide"), S.SeaLevelM(33.2, -117.4).Get(0.0), -35.11, 1e-12);
	TestEqual(TEXT("calm surface follows"), S.SurfaceHeightM(33.2, -117.4).Get(0.0), -35.11, 1e-12);
	S.SetDatumOffsetM(std::numeric_limits<double>::quiet_NaN());
	TestEqual(TEXT("non-finite datum offset -> 0"), S.GetDatumOffsetM(), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanSurfaceSourcesTest, "CamSim.Ocean.Surface.HostWavesReplaceBeaufort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanSurfaceSourcesTest::RunTest(const FString& Parameters)
{
	FOceanSurface S = MakeSurface();
	S.SetAnchor(37.8, -122.4);
	S.SetBeaufort(5.0, 270.0, 0.5);
	TestEqual(TEXT("Beaufort: 4 waves"), S.GetWaves().GetWaves().Num(), 4);

	S.SetHostWave(2, HostWave(1.0, 50.0, 90.0));
	TestTrue(TEXT("host waves active"), S.HasHostWaves());
	TestEqual(TEXT("host replaces Beaufort"), S.GetWaves().GetWaves().Num(), 1);
	TestEqual(TEXT("host wave height"), S.GetWaves().GetWaves()[0].HeightM, 1.0, 1e-12);

	S.SetHostWave(0, HostWave(0.5, 20.0, 0.0));
	TestEqual(TEXT("two host waves"), S.GetWaves().GetWaves().Num(), 2);
	TestEqual(TEXT("ordered by ID"), S.GetWaves().GetWaves()[0].LengthM, 20.0, 1e-12);

	S.SetHostWave(7, HostWave(1.0, 10.0, 0.0));   // ID >= 4: ignored
	TestEqual(TEXT("ID 7 ignored"), S.GetWaves().GetWaves().Num(), 2);
	S.SetHostWave(3, {});                          // removing an unknown ID: no-op
	TestEqual(TEXT("unknown removal no-op"), S.GetWaves().GetWaves().Num(), 2);
	S.SetHostWave(1, HostWave(1.0, 0.0, 0.0));     // zero length: dropped by SetWaves, no NaN
	TestEqual(TEXT("zero length dropped"), S.GetWaves().GetWaves().Num(), 2);
	TestTrue(TEXT("finite height"), FMath::IsFinite(S.SurfaceHeightM(37.8, -122.4).Get(TNumericLimits<double>::Max())));

	S.SetHostWave(0, {}); S.SetHostWave(1, {}); S.SetHostWave(2, {});
	TestFalse(TEXT("all removed"), S.HasHostWaves());
	TestEqual(TEXT("back to Beaufort"), S.GetWaves().GetWaves().Num(), 4);

	S.SetBeaufort(0.0, 270.0, 0.5);
	TestEqual(TEXT("Beaufort 0: calm"), S.GetWaves().GetWaves().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanSurfaceWavesTest, "CamSim.Ocean.Surface.WavesMoveTheSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanSurfaceWavesTest::RunTest(const FString& Parameters)
{
	FOceanSurface S = MakeSurface(-32.0);
	S.SetAnchor(37.8, -122.4);
	S.SetHostWave(0, HostWave(2.0, 60.0, 270.0));
	double Min = 1e9, Max = -1e9;
	for (int32 i = 0; i < 60; ++i)
	{
		S.SetTime(i * 0.2);
		const double H = S.SurfaceHeightM(37.8, -122.4).GetValue();
		Min = FMath::Min(Min, H); Max = FMath::Max(Max, H);
	}
	TestEqual(TEXT("crest ≈ sea level + a"), Max, -31.0, 0.05);
	TestEqual(TEXT("trough ≈ sea level - a"), Min, -33.0, 0.05);
	const FVector N = S.SurfaceNormalNeu(37.8, -122.4).GetValue();
	TestTrue(TEXT("normal points up"), N.Z > 0.9 && FMath::IsNearlyEqual(N.Size(), 1.0, 1e-9));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOceanSurfaceNonFiniteTest, "CamSim.Ocean.Surface.NonFiniteInputsIgnored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FOceanSurfaceNonFiniteTest::RunTest(const FString& Parameters)
{
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	FOceanSurface S = MakeSurface();
	S.SetAnchor(37.8, -122.4);
	S.SetBeaufort(3.0, 270.0, 0.5);
	const double Hs3 = S.GetWaves().SignificantHeight();
	const double From3 = S.GetWaves().GetWaves()[0].FromDeg;

	S.SetBeaufort(NaN, 270.0, 0.5);   // would clamp to Beaufort 12 (hurricane) if taken
	TestEqual(TEXT("NaN Beaufort ignored: same sea"), S.GetWaves().SignificantHeight(), Hs3, 1e-12);
	S.SetBeaufort(std::numeric_limits<double>::infinity(), NaN, NaN);
	TestEqual(TEXT("inf Beaufort / NaN direction / NaN choppiness ignored"), S.GetWaves().SignificantHeight(), Hs3, 1e-12);
	TestEqual(TEXT("direction kept"), S.GetWaves().GetWaves()[0].FromDeg, From3, 1e-12);
	S.SetBeaufort(NaN, 90.0, 0.5);    // finite fields still apply
	TestTrue(TEXT("finite direction applied"), !FMath::IsNearlyEqual(S.GetWaves().GetWaves()[0].FromDeg, From3, 1e-6));
	TestEqual(TEXT("... with the Beaufort kept"), S.GetWaves().SignificantHeight(), Hs3, 1e-12);

	S.SetWaterTempC(22.0);
	S.SetWaterTempC(NaN);
	TestEqual(TEXT("NaN water temperature ignored"), S.GetWaterTempC(), 22.0, 1e-12);
	return true;
}
