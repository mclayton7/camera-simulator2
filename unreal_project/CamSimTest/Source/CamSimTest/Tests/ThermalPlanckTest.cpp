// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Thermal/BandRadiance.h"

#include <limits>

// CamSim.Thermal.Planck.*: the in-band Planck integral and its LUT (ROADMAP 4A).

namespace
{
	constexpr double StefanBoltzmann = 5.670374419e-8;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalPlanckWholeSpectrumTest, "CamSim.Thermal.Planck.WholeSpectrumIsStefanBoltzmann",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalPlanckWholeSpectrumTest::RunTest(const FString& Parameters)
{
	for (const double T : { 200.0, 300.0, 500.0, 1000.0 })
	{
		const double Band = FBandRadiance::IntegrateBand(T, 0.1, 2000.0, 8192);
		const double Expected = StefanBoltzmann * T * T * T * T / UE_DOUBLE_PI;
		TestNearlyEqual(*FString::Printf(TEXT("0.1-2000 um at %.0f K = sigma T^4 / pi within 0.1%%"), T), Band, Expected, Expected * 1e-3);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalPlanckKnownBandsTest, "CamSim.Thermal.Planck.KnownBandValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalPlanckKnownBandsTest::RunTest(const FString& Parameters)
{
	// Reference values (independent Simpson integration, 2026-10-01): 300 K, W m^-2 sr^-1.
	TestNearlyEqual(TEXT("MWIR 3-5 um at 300 K"), FBandRadiance::IntegrateBand(300.0, 3.0, 5.0), 1.86596, 1.86596 * 1e-3);
	TestNearlyEqual(TEXT("LWIR 8-12 um at 300 K"), FBandRadiance::IntegrateBand(300.0, 8.0, 12.0), 38.5004, 38.5004 * 1e-3);
	TestEqual(TEXT("empty band"), FBandRadiance::IntegrateBand(300.0, 5.0, 3.0), 0.0);
	TestEqual(TEXT("0 K"), FBandRadiance::SpectralRadiance(4e-6, 0.0), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalPlanckLutTest, "CamSim.Thermal.Planck.LutMatchesIntegral",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalPlanckLutTest::RunTest(const FString& Parameters)
{
	struct FBand { double Lo, Hi; const TCHAR* Name; };
	for (const FBand B : { FBand{ 3.0, 5.0, TEXT("MWIR") }, FBand{ 8.0, 12.0, TEXT("LWIR") } })
	{
		FBandRadiance R;
		TestFalse(TEXT("not built"), R.IsBuilt());
		R.Build(B.Lo, B.Hi);
		TestTrue(TEXT("built"), R.IsBuilt());
		TestEqual(TEXT("lo"), R.GetLoUm(), B.Lo);
		double WorstRel = 0.0, WorstT = 0.0;
		// Mid-cell temperatures are the worst case of linear interpolation in ln B.
		const double Cell = (FThermalFrameParams::LutMaxK - FThermalFrameParams::LutMinK) / (FThermalFrameParams::LutSize - 1);
		for (int32 I = 0; I < FThermalFrameParams::LutSize - 1; I += 7)
		{
			const double T = FThermalFrameParams::LutMinK + (I + 0.5) * Cell;
			const double Exact = FBandRadiance::IntegrateBand(T, B.Lo, B.Hi);
			const double Rel = FMath::Abs(R.Radiance(static_cast<float>(T)) - Exact) / Exact;
			if (Rel > WorstRel) { WorstRel = Rel; WorstT = T; }
		}
		TestTrue(*FString::Printf(TEXT("%s LUT within 0.1%% (worst %.5f%% at %.1f K)"), B.Name, WorstRel * 100.0, WorstT), WorstRel <= 1e-3);
		TestNearlyEqual(*FString::Printf(TEXT("%s clamps below the range"), B.Name), R.Radiance(10.0f), R.Radiance(FThermalFrameParams::LutMinK), 0.0f);
		TestNearlyEqual(*FString::Printf(TEXT("%s clamps above the range"), B.Name), R.Radiance(5000.0f), R.Radiance(FThermalFrameParams::LutMaxK), 0.0f);
		TestNearlyEqual(*FString::Printf(TEXT("%s NaN -> LutMinK"), B.Name), R.Radiance(std::numeric_limits<float>::quiet_NaN()), R.Radiance(FThermalFrameParams::LutMinK), 0.0f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalPlanckMonotonicTest, "CamSim.Thermal.Planck.Monotonic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalPlanckMonotonicTest::RunTest(const FString& Parameters)
{
	FBandRadiance R;
	R.Build(3.0, 5.0);
	const float* L = R.GetLogLut();
	int32 Bad = 0;
	for (int32 I = 1; I < FThermalFrameParams::LutSize; ++I) Bad += (L[I] > L[I - 1]) ? 0 : 1;
	TestEqual(TEXT("LUT strictly increasing"), Bad, 0);
	float Prev = R.Radiance(150.0f);
	for (float T = 150.1f; T <= 1000.0f; T += 0.37f)
	{
		const float Now = R.Radiance(T);
		if (!(Now > Prev)) { AddError(FString::Printf(TEXT("not increasing at %.2f K"), T)); break; }
		Prev = Now;
	}
	return true;
}
