// Copyright CamSim Contributors. All Rights Reserved.
// Spawns ACamSimEnvironment in a test world with an ACesiumSunSky and checks the 6A wiring (ROADMAP 6A).

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Environment/CamSimEnvironment.h"
#include "Environment/SkyDomeDriver.h"
#include "Environment/Ephemeris.h"
#include "Environment/NightIllumination.h"
#include "Thermal/ThermalFrameSources.h"
#include "CesiumGeoreference.h"
#include "CesiumSunSky.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "UObject/UObjectIterator.h"

namespace
{
	constexpr double PendletonLat = 33.30, PendletonLon = -117.35;

	/** Angle in degrees between two directions. */
	double AngleDeg(const FVector& A, const FVector& B)
	{
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(A.GetSafeNormal(), B.GetSafeNormal()), -1.0, 1.0)));
	}

	/** The moon light(s): atmosphere light index 1 in World. */
	TArray<UDirectionalLightComponent*> MoonLights(UWorld* World)
	{
		TArray<UDirectionalLightComponent*> Out;
		for (TObjectIterator<UDirectionalLightComponent> It; It; ++It)
		{
			if (It->GetWorld() == World && It->IsUsedAsAtmosphereSunLight() && It->GetAtmosphereSunLightIndex() == 1) Out.Add(*It);
		}
		return Out;
	}

	FLinearColor MpcVector(UWorld* World, const TCHAR* Name)
	{
		FLinearColor V(NAN, NAN, NAN, NAN);
		const UMaterialParameterCollection* Mpc = LoadObject<UMaterialParameterCollection>(nullptr, FSkyDomeDriver::MpcPath);
		if (Mpc)
		{
			if (const UMaterialParameterCollectionInstance* I = World->GetParameterCollectionInstance(Mpc))
			{
				I->GetVectorParameterValue(FName(Name), V);
			}
		}
		return V;
	}

	FVector Xyz(const FLinearColor& C) { return FVector(C.R, C.G, C.B); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkyEnvironmentWiringTest, "CamSim.Environment.Sky.Wiring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkyEnvironmentWiringTest::RunTest(const FString&)
{
	// 1. Test world with the default georeference at Pendleton and a CesiumSunSky (as ThermalSourcesTest builds its world).
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ACesiumGeoreference* Geo = ACesiumGeoreference::GetDefaultGeoreference(World);
	if (!TestNotNull(TEXT("georeference"), Geo)) return false;
	Geo->SetOriginLongitudeLatitudeHeight(FVector(PendletonLon, PendletonLat, 0.0));   // longitude first
	ACesiumSunSky* SunSky = World->SpawnActor<ACesiumSunSky>();
	ACamSimEnvironment* Env = World->SpawnActor<ACamSimEnvironment>();
	if (!TestNotNull(TEXT("CesiumSunSky"), SunSky) || !TestNotNull(TEXT("environment"), Env)) return false;

	const FMatrix EsuToUnreal = Geo->ComputeEastSouthUpToUnrealTransformation(
		Geo->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(PendletonLon, PendletonLat, 0.0)));
	// ENU → UE world against geodesy (independent of the ESU sign convention): directions to points 10 m east / north / up.
	{
		const FVector Here = Geo->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(PendletonLon, PendletonLat, 0.0));
		const double DLat = 10.0 / 111132.0, DLon = 10.0 / (111320.0 * FMath::Cos(FMath::DegreesToRadians(PendletonLat)));
		auto To = [&](double Lon, double Lat, double H)
		{
			return (Geo->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(Lon, Lat, H)) - Here).GetSafeNormal();
		};
		TestTrue(TEXT("EnuToWorldDir east"), (FSkyDomeDriver::EnuToWorldDir(EsuToUnreal, FVector3d(1, 0, 0)) | To(PendletonLon + DLon, PendletonLat, 0.0)) > 0.99999);
		TestTrue(TEXT("EnuToWorldDir north"), (FSkyDomeDriver::EnuToWorldDir(EsuToUnreal, FVector3d(0, 1, 0)) | To(PendletonLon, PendletonLat + DLat, 0.0)) > 0.99999);
		TestTrue(TEXT("EnuToWorldDir up"), (FSkyDomeDriver::EnuToWorldDir(EsuToUnreal, FVector3d(0, 0, 1)) | To(PendletonLon, PendletonLat, 10.0)) > 0.99999);
	}

	auto Expected = [&](const FDateTime& Utc)
	{
		FNightIlluminationInput In;
		In.Ephem = FEphemeris::Compute(Utc, PendletonLat, PendletonLon, 0.0);
		return TPair<FEphemerisResult, FNightIlluminationResult>(In.Ephem, FNightIllumination::Compute(In));
	};

	// 2. Full moon, local midnight.
	const FDateTime FullMoonNight(2026, 10, 26, 7, 0);
	Env->ApplySkyForTest(FullMoonNight);
	const auto [E1, I1] = Expected(FullMoonNight);
	TestTrue(FString::Printf(TEXT("precondition: moon up at full-moon midnight (el %.2f)"), E1.MoonElDeg), E1.MoonElDeg > 10.0);
	TestTrue(FString::Printf(TEXT("precondition: sun down (el %.2f)"), E1.SunElDeg), E1.SunElDeg < -18.0);

	TArray<UDirectionalLightComponent*> Moons = MoonLights(World);
	TestEqual(TEXT("exactly one atmosphere light at index 1"), Moons.Num(), 1);
	if (Moons.Num() != 1) { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); return false; }
	UDirectionalLightComponent* Moon = Moons[0];
	TestTrue(TEXT("moon light is an atmosphere light"), Moon->IsUsedAsAtmosphereSunLight());
	TestFalse(TEXT("moon light casts no shadows on clouds"), static_cast<bool>(Moon->bCastShadowsOnClouds));
	TestTrue(TEXT("engine moon disc zeroed (the dome draws it)"), Moon->AtmosphereSunDiskColorScale.Equals(FLinearColor::Black));
	TestTrue(FString::Printf(TEXT("moon intensity %.6g == MoonToaLux %.6g"), Moon->Intensity, I1.MoonToaLux),
		FMath::IsNearlyEqual(static_cast<double>(Moon->Intensity), I1.MoonToaLux, 1e-6 * I1.MoonToaLux));
	TestTrue(TEXT("moon light visible"), Moon->IsVisible());
	const FVector MoonWorld = FSkyDomeDriver::EnuToWorldDir(EsuToUnreal, E1.MoonEnu);
	const double MoonErr = AngleDeg(Moon->GetForwardVector(), -MoonWorld);
	TestTrue(FString::Printf(TEXT("moon light points from the moon (%.5f deg)"), MoonErr), MoonErr < 0.01);
	const double MpcMoonErr = AngleDeg(Xyz(MpcVector(World, TEXT("MoonDir"))), MoonWorld);
	TestTrue(FString::Printf(TEXT("MPC MoonDir = moon direction (%.5f deg)"), MpcMoonErr), MpcMoonErr < 0.01);
	const FLinearColor MoonParams = MpcVector(World, TEXT("MoonParams"));
	TestEqual(TEXT("MPC moon enabled"), MoonParams.A, 1.0f);
	TestTrue(TEXT("MPC moon angular radius ~0.26 deg"), FMath::IsNearlyEqual(FMath::RadiansToDegrees(MoonParams.R), 0.26f, 0.02f));
	const double Radius = FMath::Asin(FSkyDomeDriver::MoonRadiusKm / E1.MoonDistanceKm);
	const double Omega = 2.0 * UE_DOUBLE_PI * (1.0 - FMath::Cos(Radius));
	const double Scale = I1.MoonToaLux / Omega / FMoonDisc::MeanShading(E1.MoonPhaseAngleDeg);
	TestTrue(FString::Printf(TEXT("MPC moon luminance scale %.6g vs %.6g"), MoonParams.G, Scale),
		FMath::IsNearlyEqual(static_cast<double>(MoonParams.G), Scale, 1e-5 * Scale));
	const FLinearColor Floor = MpcVector(World, TEXT("Floor"));
	TestTrue(TEXT("MPC floor radiance = 0.002 lux / pi"), FMath::IsNearlyEqual(static_cast<double>(Floor.R), 0.002 / UE_DOUBLE_PI, 1e-9));
	TestEqual(TEXT("MPC stars on before a packet (stars_default)"), Floor.G, 1.0f);
	TestTrue(TEXT("MPC extinction k_V"), FMath::IsNearlyEqual(Floor.B, 0.172f, 1e-6f));
	const FVector UpWorld = FSkyDomeDriver::EnuToWorldDir(EsuToUnreal, FVector3d(0, 0, 1));
	TestTrue(TEXT("MPC UpWorld"), AngleDeg(Xyz(MpcVector(World, TEXT("UpWorld"))), UpWorld) < 0.01);

	UDirectionalLightComponent* Sun = SunSky->DirectionalLight;
	TestTrue(FString::Printf(TEXT("sun intensity %.1f == SunToaLux %.1f"), Sun->Intensity, I1.SunToaLux),
		FMath::IsNearlyEqual(static_cast<double>(Sun->Intensity), I1.SunToaLux, 1e-6 * I1.SunToaLux));
	TestEqual(TEXT("sun transmittance follows its elevation (spec finding 5)"),
		SunSky->SkyAtmosphere->TransmittanceMinLightElevationAngle, -90.0f);
	TestTrue(TEXT("thermal sun lookup returns CesiumSunSky's light, not the moon"),
		CamSimThermal::FindAtmosphereSunLight(World) == Sun);
	TestTrue(FString::Printf(TEXT("GetSunElevationDeg %.3f = ephemeris %.3f"), Env->GetSunElevationDeg(), E1.SunElDeg),
		FMath::IsNearlyEqual(Env->GetSunElevationDeg(), static_cast<float>(E1.SunElDeg), 1e-3f));

	// 3. Local noon the same day: the moon is below the horizon.
	const FDateTime Noon(2026, 10, 26, 19, 0);
	Env->ApplySkyForTest(Noon);
	const auto [E2, I2] = Expected(Noon);
	TestTrue(FString::Printf(TEXT("precondition: moon down at noon (el %.2f)"), E2.MoonElDeg), E2.MoonElDeg < -1.0);
	TestEqual(TEXT("moon below the horizon: intensity 0"), Moon->Intensity, 0.0f);
	TestFalse(TEXT("moon below the horizon: hidden"), Moon->IsVisible());
	TestTrue(TEXT("thermal sun lookup still the sun by day"), CamSimThermal::FindAtmosphereSunLight(World) == Sun);
	TestFalse(TEXT("moon never casts shadows by day"), static_cast<bool>(Moon->CastShadows));

	// 4. Clock jump back to night, two weeks on: new moon.
	Env->ApplySkyForTest(FullMoonNight);
	const float FullIntensity = Moon->Intensity;
	TestTrue(TEXT("jump back to full moon re-applies the moon"), FullIntensity > 0.1f && Moon->IsVisible());
	const FDateTime NewMoon(2026, 11, 9, 7, 0);
	Env->ApplySkyForTest(NewMoon);
	const auto [E3, I3] = Expected(NewMoon);
	TestTrue(FString::Printf(TEXT("new moon: intensity %.3g < full %.3g / 100"), Moon->Intensity, FullIntensity),
		Moon->Intensity < FullIntensity / 100.0f);
	TestTrue(TEXT("new moon: MPC MoonDir follows the jump"),
		AngleDeg(Xyz(MpcVector(World, TEXT("MoonDir"))), FSkyDomeDriver::EnuToWorldDir(EsuToUnreal, E3.MoonEnu)) < 0.01);

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

// The sidereal rotation the dome's stars use (MPC_Sky SkyRot0..2): Polaris sits at the pole's elevation = latitude, a star at
// RA = LAST, Dec = latitude at the zenith; for UE axes equal to Cesium's ESU at the origin and for a rotated frame.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkySiderealRotationTest, "CamSim.Environment.Sky.SiderealRotation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkySiderealRotationTest::RunTest(const FString&)
{
	auto Eq = [](double RaDeg, double DecDeg)
	{
		const double Ra = FMath::DegreesToRadians(RaDeg), Dec = FMath::DegreesToRadians(DecDeg);
		return FVector(FMath::Cos(Dec) * FMath::Cos(Ra), FMath::Cos(Dec) * FMath::Sin(Ra), FMath::Sin(Dec));
	};
	const FMatrix Frames[] = {
		FMatrix::Identity,                                                    // UE = ESU (georeference origin)
		FScaleMatrix(100.0) * FRotationMatrix(FRotator(10.0, 33.0, -4.0)),   // away from the origin, with Cesium's cm scale
	};
	struct FCase { double Lat, Last; };
	const FCase Cases[] = { { 33.30, 0.0 }, { 33.30, 137.5 }, { 64.8, 300.0 }, { -33.9, 45.0 } };
	for (const FMatrix& M : Frames)
	{
		const FVector EastW = FSkyDomeDriver::EnuToWorldDir(M, FVector3d(1, 0, 0));
		const FVector NorthW = FSkyDomeDriver::EnuToWorldDir(M, FVector3d(0, 1, 0));
		const FVector UpW = FSkyDomeDriver::EnuToWorldDir(M, FVector3d(0, 0, 1));
		for (const FCase& C : Cases)
		{
			FVector Rows[3];
			FSkyDomeDriver::SkyRotationRows(M, C.Lat, C.Last, Rows);
			// Rows are orthonormal, so world = transpose(rows) * equatorial.
			auto ToWorld = [&](const FVector& Q) { return Rows[0] * Q.X + Rows[1] * Q.Y + Rows[2] * Q.Z; };
			TestTrue(TEXT("rows orthonormal"), FMath::IsNearlyEqual(Rows[0].Size(), 1.0, 1e-9) && FMath::IsNearlyEqual(Rows[1].Size(), 1.0, 1e-9)
				&& FMath::IsNearlyEqual(Rows[2].Size(), 1.0, 1e-9) && FMath::Abs(Rows[0] | Rows[1]) < 1e-9 && FMath::Abs(Rows[1] | Rows[2]) < 1e-9);
			const FVector Polaris = ToWorld(Eq(37.95, 89.26));
			const double El = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(Polaris | UpW, -1.0, 1.0)));
			TestTrue(FString::Printf(TEXT("lat %.1f LAST %.1f: Polaris el %.2f = latitude within 1 deg"), C.Lat, C.Last, El),
				FMath::Abs(El - C.Lat) < 1.0);
			const double Az = FMath::RadiansToDegrees(FMath::Atan2(Polaris | EastW, Polaris | NorthW));
			TestTrue(FString::Printf(TEXT("lat %.1f LAST %.1f: Polaris az %.2f within 2 deg of north"), C.Lat, C.Last, Az),
				FMath::Abs(Az) < 2.0);
			const FVector Zenith = ToWorld(Eq(C.Last, C.Lat));
			TestTrue(FString::Printf(TEXT("lat %.1f LAST %.1f: RA = LAST, Dec = lat at the zenith"), C.Lat, C.Last), (Zenith | UpW) > 1.0 - 1e-9);
			// A star on the celestial equator 90 deg east of the meridian (RA = LAST + 90) rises due east.
			const FVector EastStar = ToWorld(Eq(C.Last + 90.0, 0.0));
			TestTrue(FString::Printf(TEXT("lat %.1f LAST %.1f: RA = LAST + 90, Dec 0 due east on the horizon"), C.Lat, C.Last),
				(EastStar | EastW) > 1.0 - 1e-9);
		}
	}
	return true;
}
