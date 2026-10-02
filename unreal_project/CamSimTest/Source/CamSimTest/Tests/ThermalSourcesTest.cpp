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
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "CesiumGeoreference.h"

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

// Controller carry-over (ROADMAP 4B): the land-cover axes the capture feeds SetLandCover must be orthonormal East/North with
// the UE world's handedness (UE is left-handed: Up . (East x North) = -1); anything else is land cover off for that frame.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSourcesLandCoverAxesValidTest, "CamSim.Thermal.Sources.LandCoverAxesValid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSourcesLandCoverAxesValidTest::RunTest(const FString& Parameters)
{
	using CamSimThermal::AreLandCoverAxesValid;
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	const FVector E(1, 0, 0), N(0, -1, 0), U(0, 0, 1);   // UE world at the georeference origin: +X east, +Y south, +Z up
	TestTrue (TEXT("East-South-Up at the origin"), AreLandCoverAxesValid(E, N, U));
	const FRotator R(10.0, 33.0, -4.0);                   // any rotation keeps the handedness
	TestTrue (TEXT("rotated"), AreLandCoverAxesValid(R.RotateVector(E), R.RotateVector(N), R.RotateVector(U)));
	TestTrue (TEXT("up 0.5 deg off (camera up vs window-centre up)"),
		AreLandCoverAxesValid(E, N, FRotator(0.5, 0.0, 0.0).RotateVector(U)));
	TestFalse(TEXT("north mirrored (south)"), AreLandCoverAxesValid(E, -N, U));
	TestFalse(TEXT("east mirrored (west)"), AreLandCoverAxesValid(-E, N, U));
	TestFalse(TEXT("east/north swapped (transposed)"), AreLandCoverAxesValid(N, E, U));
	TestFalse(TEXT("east not unit"), AreLandCoverAxesValid(2.0 * E, N, U));
	TestFalse(TEXT("not at right angles"), AreLandCoverAxesValid(E, (N + 0.01 * E), U));
	TestFalse(TEXT("north along up"), AreLandCoverAxesValid(E, U, U));
	TestFalse(TEXT("NaN"), AreLandCoverAxesValid(FVector(NaN, 0, 0), N, U));
	TestFalse(TEXT("zero"), AreLandCoverAxesValid(FVector::ZeroVector, N, U));
	return true;
}

// LandCoverAxesWorld against geodesy: East/North are the directions to points 10 m east/north of the window centre, near and
// far from the georeference origin and after an origin shift (which rotates the UE axes), and pass AreLandCoverAxesValid.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FThermalSourcesLandCoverAxesWorldTest, "CamSim.Thermal.Sources.LandCoverAxesWorld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FThermalSourcesLandCoverAxesWorldTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ACesiumGeoreference* Geo = World->SpawnActor<ACesiumGeoreference>();

	struct FCase { const TCHAR* Name; double OriginLon, OriginLat, Lon, Lat; };
	const FCase Cases[] = {
		{ TEXT("SF, at origin"),             -122.46, 37.7935, -122.46,  37.7935 },
		{ TEXT("SF, window 8 km NE"),        -122.46, 37.7935, -122.39,  37.85 },
		{ TEXT("SF after origin shift"),      -77.0,  38.9,    -122.46,  37.7935 },
		{ TEXT("southern/eastern hemisphere"), 151.2, -33.9,    151.25, -33.85 },
	};
	for (const FCase& C : Cases)
	{
		Geo->SetOriginLongitudeLatitudeHeight(FVector(C.OriginLon, C.OriginLat, 0.0));
		FVector East, North;
		CamSimThermal::LandCoverAxesWorld(*Geo, C.Lat, C.Lon, East, North);
		const double DLat = 10.0 / 111132.0;
		const double DLon = 10.0 / (111320.0 * FMath::Cos(FMath::DegreesToRadians(C.Lat)));
		const FVector Here   = Geo->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(C.Lon, C.Lat, 0.0));
		const FVector ToEast = (Geo->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(C.Lon + DLon, C.Lat, 0.0)) - Here).GetSafeNormal();
		const FVector ToNorth = (Geo->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(C.Lon, C.Lat + DLat, 0.0)) - Here).GetSafeNormal();
		const FVector ToUp   = (Geo->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(C.Lon, C.Lat, 10.0)) - Here).GetSafeNormal();
		const double De = FVector::DotProduct(East, ToEast), Dn = FVector::DotProduct(North, ToNorth);
		TestTrue(FString::Printf(TEXT("%s: east (dot %.7f)"), C.Name, De), De > 0.99999);
		TestTrue(FString::Printf(TEXT("%s: north (dot %.7f)"), C.Name, Dn), Dn > 0.99999);
		const double Hand = FVector::DotProduct(ToUp, FVector::CrossProduct(East, North));
		TestTrue(FString::Printf(TEXT("%s: Up.(E x N) = %.6f (UE left-handed: -1)"), C.Name, Hand), Hand < -0.9999);
		TestTrue(FString::Printf(TEXT("%s: valid"), C.Name), CamSimThermal::AreLandCoverAxesValid(East, North, ToUp));
		FVector East2, North2, CentreUp;
		CamSimThermal::LandCoverAxesWorld(*Geo, C.Lat, C.Lon, East2, North2, &CentreUp);
		TestTrue(FString::Printf(TEXT("%s: OutUp is the geodetic up there (dot %.7f)"), C.Name, FVector::DotProduct(CentreUp, ToUp)),
			FVector::DotProduct(CentreUp, ToUp) > 0.99999 && East2.Equals(East, 0.0) && North2.Equals(North, 0.0));
	}

	// Long teleport: the camera is 3 degrees (~330 km) from a window that hasn't re-centred yet. Its up is ~3 degrees off the
	// window's, which fails the horizontality check; the Up at the window centre (what the capture component checks) passes.
	{
		Geo->SetOriginLongitudeLatitudeHeight(FVector(-122.46, 37.7935, 0.0));
		FVector East, North, CentreUp;
		CamSimThermal::LandCoverAxesWorld(*Geo, 37.7935, -122.46, East, North, &CentreUp);
		const FVector Cam = Geo->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(-119.46, 37.7935, 0.0));
		const FVector CamUp = Geo->ComputeEastSouthUpToUnrealTransformation(Cam).TransformVector(FVector(0.0, 0.0, 1.0)).GetSafeNormal();
		TestFalse(TEXT("teleport: the camera's up fails"), CamSimThermal::AreLandCoverAxesValid(East, North, CamUp));
		TestTrue (TEXT("teleport: the window-centre up passes"), CamSimThermal::AreLandCoverAxesValid(East, North, CentreUp));
	}

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}
