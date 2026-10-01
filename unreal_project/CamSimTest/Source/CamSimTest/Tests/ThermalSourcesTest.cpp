// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include <limits>
#include "Misc/AutomationTest.h"
#include "Environment/CamSimEnvironment.h"
#include "CIGI/CigiPacketTypes.h"
#include "Entity/CamSimEntity.h"
#include "Ocean/OceanWaves.h"
#include "Thermal/ThermalFrameBuilder.h"
#include "Thermal/ThermalFrameSources.h"
#include "Thermal/LandCoverWindow.h"

// CamSim.Thermal.Sources.*: world → FThermalFrameInputs helpers (ROADMAP 4A).

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSourcesAtmosphereTest, "CamSim.Thermal.Sources.AtmosphereFold",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSourcesAtmosphereTest::RunTest(const FString& Parameters)
{
	ACamSimEnvironment::FAtmosphericSnapshot S;
	FCigiAtmosphereState A;
	A.AirTemp = 31.5f; A.Visibility = 2500.0f; A.Humidity = 80;
	ACamSimEnvironment::FoldAtmosphere(S, A);
	TestEqual(TEXT("air temperature carried"), S.AirTempCelsius, 31.5f);
	TestEqual(TEXT("visibility carried"), S.AtmosphericVisibilityM, 2500.0f);
	TestTrue(TEXT("2.5 km visibility is fog"), S.bFogActive);
	A.Visibility = 40000.0f;
	ACamSimEnvironment::FoldAtmosphere(S, A);
	TestFalse(TEXT("40 km is not fog"), S.bFogActive);
	A.AirTemp = std::numeric_limits<float>::quiet_NaN();
	ACamSimEnvironment::FoldAtmosphere(S, A);
	TestEqual(TEXT("NaN air temperature ignored"), S.AirTempCelsius, 31.5f);

	FCigiWeatherState W;
	W.Coverage = 75.0f;
	ACamSimEnvironment::FoldWeather(S, W);
	TestEqual(TEXT("coverage 75 % -> 0.75"), S.CloudCover01, 0.75f);
	W.Coverage = 250.0f;
	ACamSimEnvironment::FoldWeather(S, W);
	TestEqual(TEXT("coverage clamped"), S.CloudCover01, 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSourcesSunTest, "CamSim.Thermal.Sources.SunIlluminance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSourcesSunTest::RunTest(const FString& Parameters)
{
	const FLinearColor White = FLinearColor::White;
	TestNearlyEqual(TEXT("zenith white sun = intensity"), CamSimThermal::SunIlluminanceLux(100000.0, White, White, 90.0), 100000.0, 1.0);
	TestNearlyEqual(TEXT("30 deg = half"), CamSimThermal::SunIlluminanceLux(100000.0, White, White, 30.0), 50000.0, 1.0);
	TestEqual(TEXT("below horizon = 0"), CamSimThermal::SunIlluminanceLux(100000.0, White, White, -5.0), 0.0);
	const FLinearColor Half(0.5f, 0.5f, 0.5f);
	TestNearlyEqual(TEXT("transmittance scales"), CamSimThermal::SunIlluminanceLux(100000.0, White, Half, 90.0), 50000.0, 1.0);
	TestEqual(TEXT("NaN intensity = 0"), CamSimThermal::SunIlluminanceLux(std::numeric_limits<double>::quiet_NaN(), White, White, 45.0), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSourcesWaveTest, "CamSim.Thermal.Sources.MaxWaveAmplitude",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSourcesWaveTest::RunTest(const FString& Parameters)
{
	FOceanWaves Calm;
	TestEqual(TEXT("no waves = 0"), CamSimThermal::MaxWaveAmplitudeM(Calm), 0.0);
	FOceanWaves Rough;
	Rough.SetWaves(FOceanWaves::FromBeaufort(6.0, 270.0, 0.5));   // Beaufort 6
	const double A = CamSimThermal::MaxWaveAmplitudeM(Rough);
	TestTrue(TEXT("Beaufort 6 amplitude in (0.5, 6) m"), A > 0.5 && A < 6.0);
	return true;
}

// Controller ruling R6: GatherFrameInputs hands the builder finite values only.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSourcesSanitizeTest, "CamSim.Thermal.Sources.Sanitize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSourcesSanitizeTest::RunTest(const FString& Parameters)
{
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	const double Inf = std::numeric_limits<double>::infinity();

	{
		FThermalFrameInputs In;
		TestTrue(TEXT("finite pose accepted"), CamSimThermal::SetCameraPose(In, 37.8, -122.5, 342.0));
		TestEqual(TEXT("lat"), In.CamLatDeg, 37.8);
		TestEqual(TEXT("lon"), In.CamLonDeg, -122.5);
		TestEqual(TEXT("alt"), In.CamAltHaeM, 342.0);
	}
	for (const FVector3d Bad : { FVector3d(NaN, 0, 0), FVector3d(0, Inf, 0), FVector3d(0, 0, -Inf) })
	{
		FThermalFrameInputs In;
		const FThermalFrameInputs Defaults;
		TestFalse(TEXT("non-finite pose rejected"), CamSimThermal::SetCameraPose(In, Bad.X, Bad.Y, Bad.Z));
		TestEqual(TEXT("lat default kept"), In.CamLatDeg, Defaults.CamLatDeg);
		TestEqual(TEXT("lon default kept"), In.CamLonDeg, Defaults.CamLonDeg);
		TestEqual(TEXT("alt default kept"), In.CamAltHaeM, Defaults.CamAltHaeM);
	}

	TestTrue(TEXT("up normalised"), CamSimThermal::SanitizeUpWorld(FVector(0, 0, 5)).Equals(FVector::UpVector, 1e-12));
	TestTrue(TEXT("tilted up normalised"), CamSimThermal::SanitizeUpWorld(FVector(3, 0, 4)).Equals(FVector(0.6, 0, 0.8), 1e-12));
	TestTrue(TEXT("zero up -> +Z"), CamSimThermal::SanitizeUpWorld(FVector::ZeroVector).Equals(FVector::UpVector));
	TestTrue(TEXT("NaN up -> +Z"), CamSimThermal::SanitizeUpWorld(FVector(NaN, 0, 1)).Equals(FVector::UpVector));
	TestTrue(TEXT("Inf up -> +Z"), CamSimThermal::SanitizeUpWorld(FVector(0, Inf, 1)).Equals(FVector::UpVector));

	{
		FThermalFrameInputs In;
		CamSimThermal::SetSea(In, TOptional<double>(-32.0), 0.4);
		TestTrue(TEXT("finite sea level -> sea"), In.bHasSea);
		TestEqual(TEXT("sea level carried"), In.SeaLevelHaeM, -32.0);
		TestEqual(TEXT("wave amplitude carried"), In.MaxWaveAmplitudeM, 0.4);
	}
	{
		FThermalFrameInputs In;
		CamSimThermal::SetSea(In, TOptional<double>(NaN), 0.4);
		TestFalse(TEXT("NaN sea level -> no sea"), In.bHasSea);
		CamSimThermal::SetSea(In, TOptional<double>(), 0.4);
		TestFalse(TEXT("no sea level -> no sea"), In.bHasSea);
		CamSimThermal::SetSea(In, TOptional<double>(-32.0), Inf);
		TestTrue(TEXT("sea with bad amplitude"), In.bHasSea);
		TestEqual(TEXT("non-finite amplitude -> 0"), In.MaxWaveAmplitudeM, 0.0);
	}

	TestEqual(TEXT("lux carried"), CamSimThermal::SanitizeLux(111000.0), 111000.0);
	TestEqual(TEXT("NaN lux -> 0"), CamSimThermal::SanitizeLux(NaN), 0.0);
	TestEqual(TEXT("Inf lux -> 0"), CamSimThermal::SanitizeLux(Inf), 0.0);
	TestEqual(TEXT("negative lux -> 0"), CamSimThermal::SanitizeLux(-5.0), 0.0);
	TestEqual(TEXT("NaN colour -> 0 lux"), CamSimThermal::SunIlluminanceLux(100000.0,
		FLinearColor(std::numeric_limits<float>::quiet_NaN(), 1, 1), FLinearColor::White, 45.0), 0.0);
	TestEqual(TEXT("NaN elevation -> 0 lux"), CamSimThermal::SunIlluminanceLux(100000.0, FLinearColor::White, FLinearColor::White, NaN), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSourcesSurfaceVehicleTest, "CamSim.Thermal.Sources.SurfaceVehicle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSourcesSurfaceVehicleTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("DIS land platform"),    ACamSimEntity::IsSurfaceVehicleClass({ 1, 1, 1 }, FString()));
	TestTrue(TEXT("DIS surface platform"), ACamSimEntity::IsSurfaceVehicleClass({ 1, 3, 1 }, FString()));
	TestFalse(TEXT("DIS air platform"),    ACamSimEntity::IsSurfaceVehicleClass({ 1, 2, 1 }, FString()));
	TestFalse(TEXT("munition, land domain"), ACamSimEntity::IsSurfaceVehicleClass({ 2, 1, 1 }, FString()));
	TestFalse(TEXT("unclassified"),        ACamSimEntity::IsSurfaceVehicleClass({}, FString()));
	TestTrue(TEXT("category ground"),      ACamSimEntity::IsSurfaceVehicleClass({}, TEXT("ground")));
	TestTrue(TEXT("category sea"),         ACamSimEntity::IsSurfaceVehicleClass({}, TEXT("sea")));
	TestTrue(TEXT("category Truck"),       ACamSimEntity::IsSurfaceVehicleClass({}, TEXT("Truck")));
	TestTrue(TEXT("category boat"),        ACamSimEntity::IsSurfaceVehicleClass({}, TEXT("boat")));
	TestFalse(TEXT("category character"),  ACamSimEntity::IsSurfaceVehicleClass({}, TEXT("character")));
	TestFalse(TEXT("category vehicle"),    ACamSimEntity::IsSurfaceVehicleClass({ 1, 2, 2 }, TEXT("vehicle")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSourcesLandCoverTest, "CamSim.Thermal.Sources.LandCoverInputSanitized",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSourcesLandCoverTest::RunTest(const FString& Parameters)
{
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	FLandCoverWindowData W;
	W.Id = 3;
	W.Spec.CentreLatDeg = 37.79; W.Spec.CentreLonDeg = -122.475; W.Spec.Texels = 2048; W.Spec.TexelM = 10.0f;
	W.NonZeroTexels = 100;
	FThermalFrameInputs In;
	TestTrue(TEXT("valid window accepted"), CamSimThermal::SetLandCover(In, &W, FVector(2, 0, 0), FVector(0, -3, 0)));
	TestTrue(TEXT("bValid"), In.LandCover.bValid);
	TestEqual(TEXT("id"), In.LandCover.WindowId, 3u);
	TestEqual(TEXT("centre lat"), In.LandCover.CentreLatDeg, 37.79);
	TestEqual(TEXT("texels"), In.LandCover.Texels, 2048);
	TestTrue(TEXT("axes normalised"), In.LandCover.EastWorld.Equals(FVector(1, 0, 0), 1e-12) && In.LandCover.NorthWorld.Equals(FVector(0, -1, 0), 1e-12));
	auto Rejects = [&](const FLandCoverWindowData* Win, const FVector& E, const FVector& N)
	{
		FThermalFrameInputs X;
		const bool bOk = CamSimThermal::SetLandCover(X, Win, E, N);
		return !bOk && !X.LandCover.bValid;
	};
	TestTrue(TEXT("no window"), Rejects(nullptr, FVector(1, 0, 0), FVector(0, -1, 0)));
	FLandCoverWindowData Empty = W;
	Empty.NonZeroTexels = 0;
	TestTrue(TEXT("window without data"), Rejects(&Empty, FVector(1, 0, 0), FVector(0, -1, 0)));
	FLandCoverWindowData NoId = W;
	NoId.Id = 0;
	TestTrue(TEXT("window id 0"), Rejects(&NoId, FVector(1, 0, 0), FVector(0, -1, 0)));
	TestTrue(TEXT("NaN east"), Rejects(&W, FVector(NaN, 0, 0), FVector(0, -1, 0)));
	TestTrue(TEXT("zero north"), Rejects(&W, FVector(1, 0, 0), FVector::ZeroVector));
	TestTrue(TEXT("axes not at right angles"), Rejects(&W, FVector(1, 0, 0), FVector(1, 1, 0)));
	return true;
}
