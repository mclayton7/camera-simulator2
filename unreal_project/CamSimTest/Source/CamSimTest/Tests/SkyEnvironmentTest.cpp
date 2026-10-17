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
#include "Time/SimClock.h"
#include "Misc/ScopeExit.h"

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
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};
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
	if (Moons.Num() != 1) return false;
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
	FVector Rows[3];
	FSkyDomeDriver::SkyRotationRows(EsuToUnreal, PendletonLat, E1.LocalSiderealDeg, FSkyDomeDriver::CenturiesSinceJ2000(FullMoonNight), Rows);
	const FMoonDiscFrame Frame = FSkyDomeDriver::MoonFrame(E1, EsuToUnreal, Rows,
		FSkyDomeDriver::DaysTtFromCenturiesUt(FSkyDomeDriver::CenturiesSinceJ2000(FullMoonNight)));
	const double Scale = I1.MoonToaLux / (Omega * FMoonDisc::MeanRadiance(Frame, FMoonAlbedoMap::Load(FMoonAlbedoMap::DefaultPath())));
	TestTrue(FString::Printf(TEXT("MPC moon L = L(phase) %.4f"), MoonParams.B),
		FMath::IsNearlyEqual(static_cast<double>(MoonParams.B), FMoonDisc::LunarLambertL(E1.MoonPhaseAngleDeg), 1e-6));
	TestTrue(TEXT("MPC moon north pole axis"), AngleDeg(Xyz(MpcVector(World, TEXT("MoonAxisZ"))), FVector(Frame.Axes[2])) < 0.01);
	TestTrue(TEXT("MPC moon prime meridian axis"), AngleDeg(Xyz(MpcVector(World, TEXT("MoonAxisX"))), FVector(Frame.Axes[0])) < 0.01);
	TestTrue(FString::Printf(TEXT("MPC moon luminance scale %.6g vs %.6g"), MoonParams.G, Scale),
		FMath::IsNearlyEqual(static_cast<double>(MoonParams.G), Scale, 1e-5 * Scale));
	const FLinearColor Floor = MpcVector(World, TEXT("Floor"));
	const double FloorL = 0.002 / (UE_DOUBLE_PI * FNightIllumination::FloorTransmittanceMean());
	TestTrue(TEXT("MPC floor radiance = 0.002 lux / (pi <T>)"), FMath::IsNearlyEqual(static_cast<double>(Floor.R), FloorL, 1e-6 * FloorL));
	TestEqual(TEXT("MPC stars on before a packet (stars_default)"), Floor.G, 1.0f);
	TestTrue(TEXT("MPC extinction k_V"), FMath::IsNearlyEqual(Floor.B, 0.172f, 1e-6f));
	const FVector UpWorld = FSkyDomeDriver::EnuToWorldDir(EsuToUnreal, FVector3d(0, 0, 1));
	TestTrue(TEXT("MPC UpWorld"), AngleDeg(Xyz(MpcVector(World, TEXT("UpWorld"))), UpWorld) < 0.01);

	UDirectionalLightComponent* Sun = SunSky->DirectionalLight;
	TestTrue(FString::Printf(TEXT("sun intensity %.1f == SunToaLux %.1f"), Sun->Intensity, I1.SunToaLux),
		FMath::IsNearlyEqual(static_cast<double>(Sun->Intensity), I1.SunToaLux, 1e-6 * I1.SunToaLux));
	TestEqual(TEXT("sun transmittance follows its elevation (spec finding 5)"),
		SunSky->SkyAtmosphere->TransmittanceMinLightElevationAngle, -90.0f);
	{
		// Twilight colour: ozone as the linear-sRGB band integral (red absorbed most), not UE's 680/550/440 nm samples.
		const USkyAtmosphereComponent* A = SunSky->SkyAtmosphere;
		const FLinearColor Want = FNightIllumination::OzoneAbsorptionPerKm();
		const FLinearColor Got = A->OtherAbsorption * A->OtherAbsorptionScale;
		TestTrue(FString::Printf(TEXT("ozone absorption %s = %s per km"), *Got.ToString(), *Want.ToString()),
			FMath::IsNearlyEqual(Got.R, Want.R, 1e-7f) && FMath::IsNearlyEqual(Got.G, Want.G, 1e-7f) && FMath::IsNearlyEqual(Got.B, Want.B, 1e-7f));
		TestTrue(TEXT("ozone tent unchanged (25 km, 15 km)"), A->OtherTentDistribution.TipAltitude == 25.0f && A->OtherTentDistribution.Width == 15.0f);
	}
	TestTrue(TEXT("thermal sun lookup returns CesiumSunSky's light, not the moon"),
		CamSimThermal::FindAtmosphereSunLight(World) == Sun);
	TestTrue(FString::Printf(TEXT("GetSunElevationDeg %.3f = ephemeris %.3f"), Env->GetSunElevationDeg(), E1.SunElDeg),
		FMath::IsNearlyEqual(Env->GetSunElevationDeg(), static_cast<float>(E1.SunElDeg), 1e-3f));

	// 2b. Overcast (final review I3, ruling): the moon light is dimmed by the floor's cloud factor (no moon cloud shadows),
	// so an overcast full-moon night is darker on the ground, as the log and night_check predict.
	{
		FWeatherCommand W;
		W.bEnabled = true;
		W.CoveragePct = 100.0f;
		Env->ApplyWeatherCommand(W);
		Env->ApplySkyForTest(FullMoonNight);
		const double Cloud = FNightIllumination::CloudFactor(1.0);
		TestTrue(FString::Printf(TEXT("overcast: cloud factor 0.25 (%.4f)"), Cloud), FMath::IsNearlyEqual(Cloud, 0.25, 1e-9));
		TestTrue(FString::Printf(TEXT("overcast: moon intensity %.6g == MoonToaLux x 0.25 %.6g"), Moon->Intensity, I1.MoonToaLux * Cloud),
			FMath::IsNearlyEqual(static_cast<double>(Moon->Intensity), I1.MoonToaLux * Cloud, 1e-6 * I1.MoonToaLux));
		TestTrue(TEXT("overcast: moon light still visible"), Moon->IsVisible());
		const FLinearColor FloorOvercast = MpcVector(World, TEXT("Floor"));
		TestTrue(TEXT("overcast: floor dimmed by the same factor"),
			FMath::IsNearlyEqual(static_cast<double>(FloorOvercast.R), FloorL * Cloud, 1e-6 * FloorL));
		W.bEnabled = false;
		Env->ApplyWeatherCommand(W);
		Env->ApplySkyForTest(FullMoonNight);
		TestTrue(TEXT("clear again: moon intensity back to MoonToaLux"),
			FMath::IsNearlyEqual(static_cast<double>(Moon->Intensity), I1.MoonToaLux, 1e-6 * I1.MoonToaLux));
	}

	// 3. Local noon the same day: the moon is below the horizon.
	const FDateTime Noon(2026, 10, 26, 19, 0);
	Env->ApplySkyForTest(Noon);
	const auto [E2, I2] = Expected(Noon);
	TestTrue(FString::Printf(TEXT("precondition: moon down at noon (el %.2f)"), E2.MoonElDeg), E2.MoonElDeg < -1.0);
	TestEqual(TEXT("moon below the horizon: intensity 0"), Moon->Intensity, 0.0f);
	TestFalse(TEXT("moon below the horizon: hidden"), Moon->IsVisible());
	TestTrue(TEXT("thermal sun lookup still the sun by day"), CamSimThermal::FindAtmosphereSunLight(World) == Sun);
	// Spec "Units and lights → Sun direction": the engine sun (CesiumSunSky's own solar algorithm) and FEphemeris's sun
	// (which shades the dome's moon and drives thermal / KLV) agree within 0.1 deg.
	{
		const FVector SunWorld = FSkyDomeDriver::EnuToWorldDir(EsuToUnreal, E2.SunEnu);
		const double SunErr = AngleDeg(Sun->GetForwardVector(), -SunWorld);
		TestTrue(FString::Printf(TEXT("precondition: sun up at noon (el %.2f)"), E2.SunElDeg), E2.SunElDeg > 20.0);
		TestTrue(FString::Printf(TEXT("CesiumSunSky sun light vs FEphemeris sun: %.4f deg (< 0.1)"), SunErr), SunErr < 0.1);
	}
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

	// 5. CIGI Sun Enable off: the sun light goes dark (enables re-applied; the test clock keeps the global one untouched).
	{
		FSimClock Clock;
		FCigiCelestialState Cel;   // CIGI defaults: sun and moon on
		Cel.Year = 2026; Cel.Month = 10; Cel.Day = 26; Cel.Hour = 19; Cel.Minute = 0;
		Env->ApplyCelestialControlForTest(Cel, Clock);
		Env->ApplySkyForTest(Noon);
		TestTrue(TEXT("sun enabled: lit"), Sun->Intensity > 100000.0f);
		Cel.bSunEn = false;
		Env->ApplyCelestialControlForTest(Cel, Clock);
		Env->ApplySkyForTest(Noon);
		TestEqual(TEXT("CIGI Sun Enable off: sun intensity 0"), Sun->Intensity, 0.0f);
	}
	return true;
}

// Controller ruling (Task 8 review): a Celestial Sphere Control that only changes an enable must not rewind the running
// clock to the packet's hh:mm; a date/time change still sets it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkyCelestialClockTest, "CamSim.Environment.Sky.EnableChangeKeepsClock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkyCelestialClockTest::RunTest(const FString&)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};
	ACamSimEnvironment* Env = World->SpawnActor<ACamSimEnvironment>();
	if (!TestNotNull(TEXT("environment"), Env)) return false;

	double MonoSec = 1000.0;
	FSimClock Clock;
	Clock.SetMonotonicSource([&MonoSec] { return MonoSec; });
	FCigiCelestialState Cel;
	Cel.Year = 2026; Cel.Month = 10; Cel.Day = 26; Cel.Hour = 7; Cel.Minute = 0;
	Cel.bDateVld = true; Cel.bEphemerisEn = true;   // continuous: rate 1 without a subsystem
	Env->ApplyCelestialControlForTest(Cel, Clock);
	TestTrue(FString::Printf(TEXT("%s: %s vs %s"), TEXT("packet sets the clock"), *(Clock.NowUtc()).ToIso8601(), *(FDateTime(2026, 10, 26, 7, 0)).ToIso8601()), (Clock.NowUtc()) == (FDateTime(2026, 10, 26, 7, 0)));
	MonoSec += 600.0;
	const FDateTime Advanced = Clock.NowUtc();
	TestTrue(FString::Printf(TEXT("%s: %s vs %s"), TEXT("clock runs 10 min"), *(Advanced).ToIso8601(), *(FDateTime(2026, 10, 26, 7, 10)).ToIso8601()), (Advanced) == (FDateTime(2026, 10, 26, 7, 10)));

	Cel.bStarEn = !Cel.bStarEn;
	Env->ApplyCelestialControlForTest(Cel, Clock);
	TestTrue(FString::Printf(TEXT("%s: %s vs %s"), TEXT("star-enable-only packet leaves the clock"), *(Clock.NowUtc()).ToIso8601(), *(Advanced).ToIso8601()), (Clock.NowUtc()) == (Advanced));
	Cel.StarInt = 0.3f; Cel.bMoonEn = false;
	Env->ApplyCelestialControlForTest(Cel, Clock);
	TestTrue(FString::Printf(TEXT("%s: %s vs %s"), TEXT("intensity/moon-only packet leaves the clock"), *(Clock.NowUtc()).ToIso8601(), *(Advanced).ToIso8601()), (Clock.NowUtc()) == (Advanced));

	Cel.Minute = 30;
	Env->ApplyCelestialControlForTest(Cel, Clock);
	TestTrue(FString::Printf(TEXT("%s: %s vs %s"), TEXT("a time change still sets the clock"), *(Clock.NowUtc()).ToIso8601(), *(FDateTime(2026, 10, 26, 7, 30)).ToIso8601()), (Clock.NowUtc()) == (FDateTime(2026, 10, 26, 7, 30)));
	return true;
}

// IAU 1976 precession (Meeus ch. 21) vs skyfield: Polaris J2000 (RA 37.954, Dec 89.264) on 2026-10-16 00:00 UTC, mean
// equator and equinox of date from skyfield.precessionlib.compute_precession (IAU 2006): RA 46.761274, Dec 89.374499
// (Meeus formulas in Python: RA 46.761768, Dec 89.374506, 0.03 arcsec apart; the J2000 → date shift is 0.152 deg).
// The ephemeris (LAST, moon az/el) is per observer: a camera that moves while the clock stands still (lockstep, a static
// CIGI time) must re-apply the sky, else the stars and moon keep the previous place's sky (night_check.py N4: 3 deg off
// after a launch at the config start point).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkyReapplyTest, "CamSim.Environment.Sky.ReapplyOnTimeOrObserverMove",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkyReapplyTest::RunTest(const FString&)
{
	const uint64 T0 = 1'000'000'000;
	TestTrue(TEXT("first tick"), ACamSimEnvironment::ShouldReapplySky(T0, 0, 33.3, -117.3, 0.0, 0.0));
	TestFalse(TEXT("same time, same place"), ACamSimEnvironment::ShouldReapplySky(T0 + 500'000, T0, 33.3, -117.3, 33.3, -117.3));
	TestTrue(TEXT("a sim second later"), ACamSimEnvironment::ShouldReapplySky(T0 + 1'000'000, T0, 33.3, -117.3, 33.3, -117.3));
	TestTrue(TEXT("clock went back"), ACamSimEnvironment::ShouldReapplySky(T0 - 1, T0, 33.3, -117.3, 33.3, -117.3));
	TestFalse(TEXT("moved 500 m"), ACamSimEnvironment::ShouldReapplySky(T0, T0, 33.3045, -117.3, 33.3, -117.3));
	TestTrue(TEXT("moved 2 km north, clock frozen"), ACamSimEnvironment::ShouldReapplySky(T0, T0, 33.318, -117.3, 33.3, -117.3));
	TestTrue(TEXT("moved 3 deg east, clock frozen"), ACamSimEnvironment::ShouldReapplySky(T0, T0, 33.3, -114.3, 33.3, -117.3));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkyPrecessionTest, "CamSim.Environment.Sky.Precession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkyPrecessionTest::RunTest(const FString&)
{
	auto Eq = [](double RaDeg, double DecDeg)
	{
		const double Ra = FMath::DegreesToRadians(RaDeg), Dec = FMath::DegreesToRadians(DecDeg);
		return FVector(FMath::Cos(Dec) * FMath::Cos(Ra), FMath::Cos(Dec) * FMath::Sin(Ra), FMath::Sin(Dec));
	};
	const double T = FSkyDomeDriver::CenturiesSinceJ2000(FDateTime(2026, 10, 16, 0, 0));
	TestTrue(FString::Printf(TEXT("T = %.10f"), T), FMath::IsNearlyEqual(T, 0.2678850103, 1e-9));
	FVector P[3];
	FSkyDomeDriver::PrecessionJ2000ToDate(T, P);
	const FVector J2000 = Eq(37.954, 89.264);
	const FVector Date(P[0] | J2000, P[1] | J2000, P[2] | J2000);
	const double Err = AngleDeg(Date, Eq(46.761274, 89.374499));
	TestTrue(FString::Printf(TEXT("Polaris of date within 0.005 deg of skyfield (%.6f deg)"), Err), Err < 0.005);
	TestTrue(TEXT("precession is not the identity (0.152 deg)"), FMath::Abs(AngleDeg(Date, J2000) - 0.152) < 0.002);
	FVector P0[3];
	FSkyDomeDriver::PrecessionJ2000ToDate(0.0, P0);
	TestTrue(TEXT("T = 0: identity"), P0[0].Equals(FVector(1, 0, 0), 1e-15) && P0[1].Equals(FVector(0, 1, 0), 1e-15)
		&& P0[2].Equals(FVector(0, 0, 1), 1e-15));
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
			FSkyDomeDriver::SkyRotationRows(M, C.Lat, C.Last, 0.0, Rows);   // J2000 = date: the sidereal part alone
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

			// 2026: the rows end in J2000 (the star texture): the J2000 vector of the of-date point (RA LAST, Dec lat) is at
			// the zenith, while the unprecessed one is off by the precession shift there (up to ~0.36 deg; ~0 near the
			// ecliptic pole, RA 270 Dec 66.6, which precession circles).
			const double T = FSkyDomeDriver::CenturiesSinceJ2000(FDateTime(2026, 10, 16, 0, 0));
			FVector Rows26[3], P[3];
			FSkyDomeDriver::SkyRotationRows(M, C.Lat, C.Last, T, Rows26);
			FSkyDomeDriver::PrecessionJ2000ToDate(T, P);
			const FVector ZenDate = Eq(C.Last, C.Lat);
			const FVector ZenJ2000 = P[0] * ZenDate.X + P[1] * ZenDate.Y + P[2] * ZenDate.Z;   // transpose: date → J2000
			auto ToWorld26 = [&](const FVector& Q) { return Rows26[0] * Q.X + Rows26[1] * Q.Y + Rows26[2] * Q.Z; };
			TestTrue(FString::Printf(TEXT("lat %.1f LAST %.1f 2026: precessed zenith"), C.Lat, C.Last), (ToWorld26(ZenJ2000) | UpW) > 1.0 - 1e-9);
			const double Shift = AngleDeg(ZenDate, ZenJ2000);
			const double Off = AngleDeg(ToWorld26(ZenDate), UpW);
			TestTrue(FString::Printf(TEXT("lat %.1f LAST %.1f 2026: unprecessed off by the shift (%.4f vs %.4f deg)"), C.Lat, C.Last, Off, Shift),
				FMath::Abs(Off - Shift) < 1e-5);
		}
	}
	return true;
}
