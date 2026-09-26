// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "CesiumGeoreference.h"
#include "CesiumGlobeAnchorComponent.h"
#include "Geospatial/CigiFrames.h"

// -------------------------------------------------------------------------
// CIGI heading 0 must look north everywhere — not east (UE world +X), and not
// only near the georeference origin.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCigiFramesMathTest,
	"CamSim.CigiFrames.Math",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCigiFramesMathTest::RunTest(const FString& Parameters)
{
	using namespace CamSimFrames;
	const double Tol = 1e-6;

	// ESU axes: X = East, Y = South, Z = Up. North = -Y.
	TestTrue(TEXT("Heading 0 faces north (-Y)"),
		CigiToEastSouthUp(0, 0, 0).GetForwardVector().Equals(FVector(0, -1, 0), Tol));
	TestTrue(TEXT("Heading 90 faces east (+X)"),
		CigiToEastSouthUp(90, 0, 0).GetForwardVector().Equals(FVector(1, 0, 0), Tol));
	TestTrue(TEXT("Heading 180 faces south (+Y)"),
		CigiToEastSouthUp(180, 0, 0).GetForwardVector().Equals(FVector(0, 1, 0), Tol));
	TestTrue(TEXT("Pitch +30 raises the nose"),
		CigiToEastSouthUp(0, 30, 0).GetForwardVector().Z > 0.49);
	TestTrue(TEXT("Heading 0, roll +20 lowers the right (east) wing"),
		CigiToEastSouthUp(0, 0, 20).GetRightVector().Z < -0.33);

	for (const FVector& Hpr : { FVector(0, 0, 0), FVector(37, -12, 5), FVector(271.5, 45, -60), FVector(359, 0, 0) })
	{
		const FRotator Back = EastSouthUpToCigi(CigiToEastSouthUp(Hpr.X, Hpr.Y, Hpr.Z));
		TestTrue(FString::Printf(TEXT("Round trip %s -> %s"), *Hpr.ToString(), *Back.ToString()),
			FMath::IsNearlyEqual(FRotator::NormalizeAxis(Back.Yaw - Hpr.X), 0.0, 1e-3)
			&& FMath::IsNearlyEqual(Back.Pitch, Hpr.Y, 1e-3)
			&& FMath::IsNearlyEqual(Back.Roll, Hpr.Z, 1e-3));
	}

	// Dead reckoning: nose up 10°, 100 m/s forward, CIGI Z down = 0 -> climbs.
	const FVector Vel = BodyVelocityToNeu(CigiToNeu(90, 10, 0), FVector(100, 0, 0));
	TestTrue(FString::Printf(TEXT("Pitched-up flight climbs (up = %.2f)"), Vel.Z), Vel.Z > 17.0);
	TestTrue(TEXT("Heading 90 moves east"), Vel.Y > 98.0 && FMath::Abs(Vel.X) < 1e-6);
	const FVector Sink = BodyVelocityToNeu(CigiToNeu(0, 0, 0), FVector(0, 0, 5));
	TestTrue(TEXT("CIGI body Z+ (down) descends"), FMath::IsNearlyEqual(Sink.Z, -5.0, 1e-6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCigiFramesGlobeTest,
	"CamSim.CigiFrames.HeadingNorthAwayFromOrigin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCigiFramesGlobeTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);

	ACesiumGeoreference* Geo = World->SpawnActor<ACesiumGeoreference>();
	Geo->SetOriginLongitudeLatitudeHeight(FVector(-77.0, 38.9, 0.0));

	AActor* Actor = World->SpawnActor<AActor>();
	USceneComponent* Root = NewObject<USceneComponent>(Actor);
	Actor->SetRootComponent(Root);
	Root->RegisterComponent();
	UCesiumGlobeAnchorComponent* Anchor = NewObject<UCesiumGlobeAnchorComponent>(Actor);
	Anchor->SetGeoreference(Geo);
	Anchor->RegisterComponent();

	struct FCase { const TCHAR* Name; double Lon, Lat; };
	const FCase Cases[] = {
		{ TEXT("at origin"),           -77.0,  38.9 },
		{ TEXT("~500 km east"),        -71.2,  38.9 },
		{ TEXT("latitude 60"),         -77.0,  60.0 },
		{ TEXT("equator"),             -77.0,   0.0 },
		{ TEXT("other hemisphere"),    151.2, -33.9 },
	};
	for (const FCase& C : Cases)
	{
		for (const double Heading : { 0.0, 90.0, 225.0 })
		{
			Anchor->MoveToLongitudeLatitudeHeight(FVector(C.Lon, C.Lat, 1000.0));
			Anchor->SetEastSouthUpRotation(CamSimFrames::CigiToEastSouthUp(Heading, 0.0, 0.0));

			// Expected direction in UE world: towards a point 10 m away along the heading.
			const double Rad  = FMath::DegreesToRadians(Heading);
			const double DLat = 10.0 * FMath::Cos(Rad) / 111320.0;
			const double DLon = 10.0 * FMath::Sin(Rad) / (111320.0 * FMath::Cos(FMath::DegreesToRadians(C.Lat)));
			const FVector Here  = Geo->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(C.Lon, C.Lat, 1000.0));
			const FVector There = Geo->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(C.Lon + DLon, C.Lat + DLat, 1000.0));
			const FVector Expected = (There - Here).GetSafeNormal();

			const double Dot = FVector::DotProduct(Actor->GetActorForwardVector(), Expected);
			TestTrue(FString::Printf(TEXT("%s, heading %.0f: forward matches (dot %.5f)"), C.Name, Heading, Dot),
				Dot > 0.9999);
		}
	}

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

// -------------------------------------------------------------------------
// CIGI Rate Control coordinate system (ICD 3.3 section 4.1.8): Local rates are
// body-frame; World/Parent rates are North/East/Down and heading/pitch/roll
// rates, whatever the entity's attitude.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCigiFramesRateControlTest,
	"CamSim.CigiFrames.RateControlFrames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCigiFramesRateControlTest::RunTest(const FString& Parameters)
{
	using namespace CamSimFrames;

	// Metres moved north/east/up between two poses (small displacements).
	auto Moved = [](const FGeoPose& A, const FGeoPose& B)
	{
		const double MPerDegLat = 111132.0;
		const double MPerDegLon = 111320.0 * FMath::Cos(FMath::DegreesToRadians(A.Lat));
		return FVector((B.Lat - A.Lat) * MPerDegLat, (B.Lon - A.Lon) * MPerDegLon, B.Alt - A.Alt);
	};

	FGeoPose Start;
	Start.Lat = 45.0;
	Start.Lon = 10.0;
	Start.Alt = 1000.0;
	Start.Neu = CigiToNeu(90.0, 0.0, 0.0);  // facing east

	FGeoPose Local = Start;
	IntegrateRates(Local, FVector(100, 0, 10), FVector::ZeroVector, /*bLocalFrame=*/true, 1.0);
	const FVector LocalMove = Moved(Start, Local);
	TestTrue(FString::Printf(TEXT("Local X rate moves along the nose (east): %s"), *LocalMove.ToString()),
		LocalMove.Equals(FVector(0, 100, -10), 1.0));

	FGeoPose World = Start;
	IntegrateRates(World, FVector(100, 0, 10), FVector::ZeroVector, /*bLocalFrame=*/false, 1.0);
	const FVector WorldMove = Moved(Start, World);
	TestTrue(FString::Printf(TEXT("World X rate moves north regardless of heading: %s"), *WorldMove.ToString()),
		WorldMove.Equals(FVector(100, 0, -10), 1.0));

	// World angular rates are Euler-angle rates: a yaw rate while pitched up
	// changes heading only.
	FGeoPose Turning = Start;
	Turning.Neu = CigiToNeu(90.0, 30.0, 0.0);
	IntegrateRates(Turning, FVector::ZeroVector, FVector(0, 0, 10), /*bLocalFrame=*/false, 1.0);
	const FRotator Hpr = Turning.Neu.Rotator();
	TestTrue(FString::Printf(TEXT("World yaw rate: heading 100, pitch 30, roll 0 (got %s)"), *Hpr.ToString()),
		FMath::IsNearlyEqual(Hpr.Yaw, 100.0, 1e-3) && FMath::IsNearlyEqual(Hpr.Pitch, 30.0, 1e-3)
		&& FMath::IsNearlyZero(Hpr.Roll, 1e-3));

	// A body yaw rate while pitched up is not the same motion.
	FGeoPose BodyTurn = Start;
	BodyTurn.Neu = CigiToNeu(90.0, 30.0, 0.0);
	IntegrateRates(BodyTurn, FVector::ZeroVector, FVector(0, 0, 10), /*bLocalFrame=*/true, 1.0);
	TestFalse(TEXT("Local yaw rate while pitched up also rolls the entity"),
		FMath::IsNearlyZero(BodyTurn.Neu.Rotator().Roll, 0.1));
	return true;
}

// -------------------------------------------------------------------------
// Geodetic -> local and entity-frame conversions invert their forward forms.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCigiFramesInverseTest,
	"CamSim.CigiFrames.GeodeticInverse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCigiFramesInverseTest::RunTest(const FString& Parameters)
{
	using namespace CamSimFrames;

	double Lat, Lon, Alt;
	const FVector Neu(1234.5, -678.25, 90.0);
	OffsetGeodetic(47.0, 8.0, 500.0, Neu, Lat, Lon, Alt);
	TestTrue(TEXT("GeodeticDeltaToNeu inverts OffsetGeodetic"),
		GeodeticDeltaToNeu(47.0, 8.0, 500.0, Lat, Lon, Alt).Equals(Neu, 1e-6));

	const FVector AcrossDateLine = GeodeticDeltaToNeu(0.0, 179.9999, 0.0, 0.0, -179.9999, 0.0);
	TestTrue(FString::Printf(TEXT("Delta across 180 deg is ~22 m east (%s)"), *AcrossDateLine.ToString()),
		FMath::IsNearlyEqual(AcrossDateLine.Y, 22.26, 0.05) && FMath::Abs(AcrossDateLine.X) < 1e-6);

	FGeoPose Entity;
	Entity.Lat = -33.9; Entity.Lon = 151.2; Entity.Alt = 1200.0;
	Entity.Neu = CigiToNeu(37.0, 10.0, -20.0);
	const FVector Offset(120.0, -45.0, 8.0);
	BodyOffsetToGeodetic(Entity, Offset, Lat, Lon, Alt);
	const FVector Back = GeodeticToBodyOffset(Entity, Lat, Lon, Alt);
	TestTrue(FString::Printf(TEXT("GeodeticToBodyOffset inverts BodyOffsetToGeodetic (%s)"), *Back.ToString()),
		Back.Equals(Offset, 1e-3));

	double Az, El;
	NeuToAzEl(FVector(1, 0, 0), Az, El);
	TestTrue(TEXT("north = az 0, el 0"), FMath::IsNearlyZero(Az, 1e-9) && FMath::IsNearlyZero(El, 1e-9));
	NeuToAzEl(FVector(0, 1, 0), Az, El);
	TestTrue(TEXT("east = az 90"), FMath::IsNearlyEqual(Az, 90.0, 1e-9));
	NeuToAzEl(FVector(-1, -1, 0), Az, El);
	TestTrue(TEXT("south-west = az -135"), FMath::IsNearlyEqual(Az, -135.0, 1e-9));
	NeuToAzEl(FVector(0, 0, 1), Az, El);
	TestTrue(TEXT("up = el 90"), FMath::IsNearlyEqual(El, 90.0, 1e-9));
	return true;
}
