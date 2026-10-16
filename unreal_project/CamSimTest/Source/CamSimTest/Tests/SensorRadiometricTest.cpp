// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Sensor/Radiometry.h"
#include "Sensor/SensorPresets.h"
#include "Sensor/SensorController.h"
#include "Sensor/SensorReference.h"
#include "Config/CamSimConfig.h"
#include "Tests/SensorPhysicsHelpers.h"

// Radiometric EO exposure (ROADMAP 9.4, 6A): photon gain = R · t_int / FullWellE with R from optics, QE and a 5800 K
// photons-per-lumen constant; the AE's photon-stage limits come from the integration time range.
namespace
{
	FSensorModeConfig RadiometricEo() { FSensorModeConfig M; CamSimSensorPresets::Apply(TEXT("eo_hd_cmos"), M); return M; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRadiometricConstantsTest, "CamSim.Sensor.Radiometric.Constants",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FRadiometricConstantsTest::RunTest(const FString&)
{
	const double Ppl = CamSimRadiometry::PhotonsPerLumen(0.4f, 0.7f);
	AddInfo(FString::Printf(TEXT("photons/lm 0.4-0.7 um at 5800 K: %.4g"), Ppl));
	TestTrue(TEXT("photons/lm 0.4-0.7 µm ~1.1-1.3e16"), Ppl > 1.0e16 && Ppl < 1.4e16);
	const FSensorModeConfig M = RadiometricEo();
	// By hand: π/(4·16) · 0.9 · (2.9e-6)² · 0.6 · Ppl, with the preset's values as stored (float).
	const double Tau = 0.9f, PitchM = 2.9f * 1e-6, Qe = 0.6f;
	const double R = UE_DOUBLE_PI / 64.0 * Tau * PitchM * PitchM * Qe * Ppl;
	TestTrue(TEXT("R"), FMath::IsNearlyEqual(CamSimRadiometry::ElectronsPerSecondPerNit(M), R, R * 1e-9));
	const FSensorExposureConfig E = CamSimRadiometry::EffectiveExposure(M);
	AddInfo(FString::Printf(TEXT("R %.1f e/s per cd/m^2, photon gain %.2f .. %.2f EV"), R, E.MinGainEv, E.MaxPhotonGainEv));
	TestTrue(TEXT("max photon gain = log2(R tmax / FW)"), FMath::IsNearlyEqual(E.MaxPhotonGainEv, (float)FMath::Log2(R * 0.0333 / 10000.0), 1e-3f));
	TestTrue(TEXT("max ≈ -6.8 EV"), E.MaxPhotonGainEv > -7.5f && E.MaxPhotonGainEv < -6.0f);
	TestTrue(TEXT("min = log2(R tmin / FW)"), FMath::IsNearlyEqual(E.MinGainEv, (float)FMath::Log2(R * 10e-6 / 10000.0), 1e-3f));
	TestTrue(TEXT("target/lag kept"), E.TargetGrey == M.Exposure.TargetGrey && E.LagFrames == M.Exposure.LagFrames);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRadiometricShotNoiseTest, "CamSim.Sensor.Radiometric.ShotNoise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FRadiometricShotNoiseTest::RunTest(const FString&)
{
	// A flat field at L cd/m² with t_int = 10 ms: electrons = R · L · t; temporal std in electrons ≈ √e (read 2 e).
	FSensorModeConfig M = RadiometricEo();
	const double R = CamSimRadiometry::ElectronsPerSecondPerNit(M);
	const double L = 0.5, T = 0.010;
	const double ExpectedE = R * L * T;
	const float PhotonGain = static_cast<float>(R * T / M.Detector.FullWellE);
	TestTrue(TEXT("t from gain"), FMath::IsNearlyEqual(CamSimRadiometry::IntegrationSeconds(M, PhotonGain), T, 1e-9));
	// The CPU reference's electron path on a 64×64 flat field over 32 frames (Tests/SensorPhysicsHelpers.h).
	const double MeasuredStdE = CamSimSensorPhysicsTest::TemporalStdElectrons(M, /*Signal=*/static_cast<float>(L), PhotonGain, 64, 32);
	const double Predicted = FMath::Sqrt(ExpectedE + M.Detector.ReadNoiseE * M.Detector.ReadNoiseE);
	TestTrue(FString::Printf(TEXT("shot noise %.2f vs %.2f e"), MeasuredStdE, Predicted), FMath::Abs(MeasuredStdE / Predicted - 1.0) < 0.05);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRadiometricAeOrderTest, "CamSim.Sensor.Radiometric.IntegrationBeforeAnalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FRadiometricAeOrderTest::RunTest(const FString&)
{
	FSensorModeConfig M = RadiometricEo();
	FSensorController C;
	// A dim scene: median 2^-6 cd/m² → wants more than the photon cap → t_int = t_max, analog > 1.
	FSensorHistogram H; H.Bins[FMath::Clamp(int32((-6.0f - FSensorHistogram::MinLog2) * FSensorHistogram::BinsPerStop), 0, FSensorHistogram::NumBins - 1)] = 1000;
	FSensorControllerInput In; In.Mode = ESensorGraphMode::EO; In.NewHistogram = &H; In.bCameraCut = true; In.DeltaSimSec = 1.0 / 30.0;
	FSensorFrameParams P = C.Update(In, M);
	for (int32 I = 0; I < 30; ++I) { In.bCameraCut = false; In.Serial = I + 1; P = C.Update(In, M); }
	TestTrue(TEXT("t_int at max"), FMath::IsNearlyEqual(C.GetIntegrationMs(), 33.3f, 0.05f));
	TestTrue(TEXT("analog > 1"), P.AnalogGain > 1.0f);
	TestTrue(TEXT("dark integrates over t_int"), FMath::IsNearlyEqual(P.DarkE, M.Detector.DarkCurrentEs * 0.0333f, 1e-4f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRadiometricLegacyKeysTest, "CamSim.Sensor.Radiometric.LegacyKeysIgnored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FRadiometricLegacyKeysTest::RunTest(const FString&)
{
	AddExpectedError(TEXT("is ignored on a radiometric mode"), EAutomationExpectedErrorFlags::Contains, 1);
	const FCamSimConfig Cfg = FCamSimConfig::LoadFromYamlString(TEXT("sensor_modes:\n  eo:\n    exposure:\n      max_photon_gain_ev: -2\n"));
	const FSensorModeConfig& M = Cfg.SensorModeConfigs.FindChecked(ESensorMode::EO);
	TestTrue(TEXT("radiometric"), CamSimRadiometry::IsRadiometric(M));
	TestTrue(TEXT("cap from optics, not -2"), CamSimRadiometry::EffectiveExposure(M).MaxPhotonGainEv < -6.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRadiometricIrTest, "CamSim.Sensor.Radiometric.IrStaysCalibrated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FRadiometricIrTest::RunTest(const FString&)
{
	for (const TCHAR* P : { TEXT("mwir_cooled"), TEXT("lwir_uncooled") })
	{
		FSensorModeConfig M; CamSimSensorPresets::Apply(P, M);
		TestFalse(FString(P) + TEXT(" not radiometric"), CamSimRadiometry::IsRadiometric(M));
		TestEqual(FString(P) + TEXT(" exposure unchanged"), CamSimRadiometry::EffectiveExposure(M).MaxPhotonGainEv, M.Exposure.MaxPhotonGainEv);
	}
	return true;
}
