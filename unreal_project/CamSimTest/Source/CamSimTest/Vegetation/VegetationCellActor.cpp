// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationCellActor.h"

#include "CesiumGlobeAnchorComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "CamSimTest.h"

ACamSimVegetationCell::ACamSimVegetationCell()
{
	PrimaryActorTick.bCanEverTick = false;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);   // the globe anchor moves it on origin shifts
	SetRootComponent(Root);
	GlobeAnchor = CreateDefaultSubobject<UCesiumGlobeAnchorComponent>(TEXT("GlobeAnchor"));
	Trees = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("Trees"));
	Trees->SetupAttachment(Root);
	Trees->SetMobility(EComponentMobility::Movable);
	Trees->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Trees->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	Trees->SetCanEverAffectNavigation(false);
	Trees->SetRenderCustomDepth(false);
}

void ACamSimVegetationCell::Init(double CentreLatDeg, double CentreLonDeg, double BaseHeightM, UStaticMesh* Mesh,
	const TArray<FTransform>& LocalInstances, bool bCastShadows, float CullDistanceM)
{
	if (!Mesh)
	{
		static bool bLogged = false;
		if (!bLogged)
		{
			bLogged = true;
			UE_LOG(LogCamSim, Warning, TEXT("Vegetation: cell spawned without a tree mesh; it stays empty (logged once)"));
		}
		return;
	}
	GlobeAnchor->MoveToLongitudeLatitudeHeight(FVector(CentreLonDeg, CentreLatDeg, BaseHeightM));
	GlobeAnchor->SetEastSouthUpRotation(FQuat::Identity);
	Trees->SetCastShadow(bCastShadows);
	// Clamped in double before the int32 conversion (radius_m comes from config); non-finite = no cut.
	const double CullCmD = FMath::IsFinite(CullDistanceM) ? FMath::Clamp(static_cast<double>(CullDistanceM) * 100.0, 1.0, static_cast<double>(MAX_int32)) : static_cast<double>(MAX_int32);
	const int32 CullCm = static_cast<int32>(CullCmD);
	Trees->SetCullDistances(0, CullCm);
	Trees->SetStaticMesh(Mesh);
	Trees->AddInstances(LocalInstances, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false, /*bUpdateNavigation=*/false);
}

int32 ACamSimVegetationCell::GetInstanceCount() const
{
	return Trees ? Trees->GetInstanceCount() : 0;
}
