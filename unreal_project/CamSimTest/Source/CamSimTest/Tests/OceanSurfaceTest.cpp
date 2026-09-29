// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Ocean/OceanSurface.h"

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
