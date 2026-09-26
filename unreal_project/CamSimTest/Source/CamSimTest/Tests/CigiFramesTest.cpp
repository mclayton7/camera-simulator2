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
