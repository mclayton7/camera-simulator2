// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/ThermalMaterials.h"
#include "Thermal/ThermalModel.h"

// CamSim.Thermal.Model.*: the closed-form surface temperature (ROADMAP 4A).

namespace
{
	constexpr int32 N = FThermalModel::NumSamples;

	FThermalSite SanFrancisco(int32 DayOfYear)
	{
		FThermalSite S;
		S.Year = 2026; S.DayOfYear = DayOfYear; S.LatDeg = 37.80; S.LonDeg = -122.45;
		S.TairMeanK = 288.15; S.AirSwingK = 8.0; S.Cloud = 0.0;
		return S;
	}

	/** Smooth forcing: exactly representable by NumHarmonics harmonics. Peak of the first harmonic at PeakHour. */
	TArray<double> CosineForcing(double Mean, double Amp, double PeakHour)
	{
		TArray<double> F;
		for (int32 K = 0; K < N; ++K)
		{
			const double Th = 2.0 * UE_DOUBLE_PI * K / N;
			F.Add(Mean + Amp * FMath::Cos(Th - 2.0 * UE_DOUBLE_PI * PeakHour / 24.0) + 0.1 * Amp * FMath::Cos(3.0 * Th));
		}
		return F;
	}

	/** (min, max, hour of max) of a response over a day at 1-minute steps. */
	FVector3d DailyRange(TFunctionRef<double(double)> T)
	{
		double Lo = TNumericLimits<double>::Max(), Hi = -Lo, HiHour = 0.0;
		for (int32 M = 0; M < 1440; ++M)
		{
			const double V = T(M * 60.0);
			Lo = FMath::Min(Lo, V);
			if (V > Hi) { Hi = V; HiHour = M / 60.0; }
		}
		return FVector3d(Lo, Hi, HiHour);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalModelEquilibriumTest, "CamSim.Thermal.Model.ZeroInertiaTracksEquilibrium",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalModelEquilibriumTest::RunTest(const FString& Parameters)
{
	const TArray<double> F = CosineForcing(5000.0, 300.0, 13.0);
	const FThermalModel::FHarmonics H = FThermalModel::Fit(F);
	const double Hc = 15.0;
	for (int32 K = 0; K < N; K += 5)
	{
		TestNearlyEqual(*FString::Printf(TEXT("I = 0 tracks F/h at sample %d"), K), FThermalModel::Response(H, Hc, 0.0, K * 900.0), F[K] / Hc, 1e-9);
	}
	// Real forcing (sunrise kink, so only up to the 6-harmonic truncation): noon within 2 K.
	const FThermalSite Site = SanFrancisco(355);
	FThermalMaterial Soil = FThermalMaterialTable::BuiltIns()[FThermalMaterialTable::TerrainDefault];
	Soil.ThermalInertia = 0.0f;
	TArray<double> Fr;
	for (int32 K = 0; K < N; ++K) Fr.Add(FThermalModel::Forcing(Site, Soil, K * 24.0 / N));
	const double Hs = FThermalModel::ExchangeCoefficient(Site, Soil);
	TestNearlyEqual(TEXT("real forcing, noon"), FThermalModel::Response(FThermalModel::Fit(Fr), Hs, 0.0, 43200.0), Fr[N / 2] / Hs, 2.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalModelInertiaTest, "CamSim.Thermal.Model.InertiaDampsAndDelays",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalModelInertiaTest::RunTest(const FString& Parameters)
{
	const FThermalModel::FHarmonics H = FThermalModel::Fit(CosineForcing(5000.0, 400.0, 12.0));
	double PrevAmp = TNumericLimits<double>::Max(), PrevPeak = -1.0;
	for (const double I : { 0.0, 500.0, 1500.0, 3000.0 })
	{
		const FVector3d R = DailyRange([&](double T) { return FThermalModel::Response(H, 15.0, I, T); });
		const double Amp = 0.5 * (R.Y - R.X);
		TestTrue(*FString::Printf(TEXT("amplitude falls with inertia (I %.0f: %.3f K)"), I, Amp), Amp < PrevAmp);
		TestTrue(*FString::Printf(TEXT("peak moves later with inertia (I %.0f: %.2f h)"), I, R.Z), R.Z > PrevPeak);
		PrevAmp = Amp;
		PrevPeak = R.Z;
	}
	TestTrue(TEXT("I = 3000 lags noon by more than 1.5 h"), PrevPeak > 13.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalModelMeanTest, "CamSim.Thermal.Model.DailyMeanIsF0OverH",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalModelMeanTest::RunTest(const FString& Parameters)
{
	const FThermalSite Site = SanFrancisco(172);
	const FThermalMaterial Soil = FThermalMaterialTable::BuiltIns()[FThermalMaterialTable::TerrainDefault];
	TArray<double> F;
	for (int32 K = 0; K < N; ++K) F.Add(FThermalModel::Forcing(Site, Soil, K * 24.0 / N));
	const FThermalModel::FHarmonics H = FThermalModel::Fit(F);
	const double Hs = FThermalModel::ExchangeCoefficient(Site, Soil);
	for (const double I : { 0.0, 800.0, 2500.0 })
	{
		double Mean = 0.0;
		for (int32 M = 0; M < 1440; ++M) Mean += FThermalModel::Response(H, Hs, I, M * 60.0) / 1440.0;
		TestNearlyEqual(*FString::Printf(TEXT("daily mean = F0 / h at I %.0f"), I), Mean, H.Re[0] / Hs, 1e-6);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalModelDeterminismTest, "CamSim.Thermal.Model.DeterministicUnderClockJumps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalModelDeterminismTest::RunTest(const FString& Parameters)
{
	const FThermalMaterialTable Table;
	FThermalModel A, B;
	TestTrue(TEXT("first update fits"), A.Update(SanFrancisco(355), Table));
	const double Noon = A.TemperatureK(FThermalMaterialTable::TerrainDefault, 43200.0, 288.15);
	A.TemperatureK(FThermalMaterialTable::TerrainDefault, 3600.0, 288.15);
	A.TemperatureK(FThermalMaterialTable::TerrainDefault, 86000.0, 288.15);
	TestEqual(TEXT("no history: same time, same value after jumps"), A.TemperatureK(FThermalMaterialTable::TerrainDefault, 43200.0, 288.15), Noon);
	FThermalSite Other = SanFrancisco(172);
	B.Update(Other, Table);
	B.Update(SanFrancisco(355), Table);
	TestEqual(TEXT("a model that saw another day first agrees"), B.TemperatureK(FThermalMaterialTable::TerrainDefault, 43200.0, 288.15), Noon);
	TestFalse(TEXT("identical site: no refit"), A.Update(SanFrancisco(355), Table));
	FThermalSite Near = SanFrancisco(355); Near.LatDeg += 0.4;
	TestFalse(TEXT("0.4 deg move: no refit"), A.Update(Near, Table));
	FThermalSite Far = SanFrancisco(355); Far.LatDeg += 0.6;
	TestTrue(TEXT("0.6 deg move: refit"), A.Update(Far, Table));
	FThermalSite Cloudy = Far; Cloudy.Cloud = 0.3;
	TestTrue(TEXT("cloud change: refit"), A.Update(Cloudy, Table));
	FThermalSite Warm = Cloudy; Warm.TairMeanK += 1.0;
	TestTrue(TEXT("air temperature change: refit"), A.Update(Warm, Table));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalModelCrossoverTest, "CamSim.Thermal.Model.CrossoverWaterVsSoil",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalModelCrossoverTest::RunTest(const FString& Parameters)
{
	const FThermalMaterialTable Table;
	FThermalModel M;
	M.Update(SanFrancisco(355), Table);
	const double WaterK = 288.15;
	const double SoilNoon = M.TemperatureK(FThermalMaterialTable::TerrainDefault, 12.0 * 3600.0, WaterK);
	const double SoilNight = M.TemperatureK(FThermalMaterialTable::TerrainDefault, 4.0 * 3600.0, WaterK);
	const double WaterNoon = M.TemperatureK(FThermalMaterialTable::Water, 12.0 * 3600.0, WaterK);
	const double WaterNight = M.TemperatureK(FThermalMaterialTable::Water, 4.0 * 3600.0, WaterK);
	TestTrue(*FString::Printf(TEXT("noon: soil %.1f K warmer than water %.1f K"), SoilNoon, WaterNoon), SoilNoon > WaterNoon + 2.0);
	TestTrue(*FString::Printf(TEXT("04:00: soil %.1f K cooler than water %.1f K"), SoilNight, WaterNight), SoilNight < WaterNight - 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalModelCloudsTest, "CamSim.Thermal.Model.CloudsDampAmplitude",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalModelCloudsTest::RunTest(const FString& Parameters)
{
	const FThermalMaterialTable Table;
	FThermalModel Clear, Overcast;
	FThermalSite S = SanFrancisco(172);
	Clear.Update(S, Table);
	S.Cloud = 0.9;
	Overcast.Update(S, Table);
	const FVector3d C = DailyRange([&](double T) { return Clear.TemperatureK(FThermalMaterialTable::TerrainDefault, T, 288.15); });
	const FVector3d O = DailyRange([&](double T) { return Overcast.TemperatureK(FThermalMaterialTable::TerrainDefault, T, 288.15); });
	TestTrue(*FString::Printf(TEXT("overcast range %.1f K < clear range %.1f K"), O.Y - O.X, C.Y - C.X), (O.Y - O.X) < (C.Y - C.X));
	TestNearlyEqual(TEXT("Kasten-Czeplak factor at c = 1"), FThermalModel::CloudFactor(1.0), 0.25, 1e-12);
	TestEqual(TEXT("no sun below the horizon"), FThermalModel::ClearSkyGhi(-3.0), 0.0);
	TestNearlyEqual(TEXT("Haurwitz at the zenith"), FThermalModel::ClearSkyGhi(90.0), 1098.0 * FMath::Exp(-0.057), 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalModelFixedTest, "CamSim.Thermal.Model.FixedTemperatureClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalModelFixedTest::RunTest(const FString& Parameters)
{
	const FThermalMaterialTable Table;
	FThermalModel M;
	M.Update(SanFrancisco(172), Table);
	const double WaterK = 285.0;
	const FVector3d R = DailyRange([&](double T) { return M.TemperatureK(FThermalMaterialTable::Water, T, WaterK); });
	TestNearlyEqual(TEXT("water min"), R.X, WaterK - FThermalModel::WaterSwingK, 1e-3);
	TestNearlyEqual(TEXT("water max"), R.Y, WaterK + FThermalModel::WaterSwingK, 1e-3);
	TestNearlyEqual(TEXT("water peaks at 15:00"), R.Z, FThermalModel::AirPeakHour, 0.02);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalModelPolarTest, "CamSim.Thermal.Model.PolarDayAndNight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalModelPolarTest::RunTest(const FString& Parameters)
{
	// Review focus 3: a sun that never sets (S > 0 all day) or never rises (S = 0 all day).
	const FThermalMaterialTable Table;
	for (const int32 Doy : { 172, 355 })
	{
		FThermalSite S = SanFrancisco(Doy);
		S.LatDeg = 80.0;
		S.TairMeanK = 273.15;
		FThermalModel M;
		M.Update(S, Table);
		for (int32 C = 0; C < Table.Num(); ++C)
		{
			const FVector3d R = DailyRange([&](double T) { return M.TemperatureK(C, T, 272.0); });
			TestTrue(*FString::Printf(TEXT("day %d class %s finite and in [150, 400] K (%.1f..%.1f)"), Doy, *Table.Get(C).Name, R.X, R.Y),
				FMath::IsFinite(R.X) && FMath::IsFinite(R.Y) && R.X >= 150.0 && R.Y <= 400.0);
		}
	}
	return true;
}
