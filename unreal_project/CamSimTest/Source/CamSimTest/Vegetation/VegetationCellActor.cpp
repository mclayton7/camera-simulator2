// Copyright CamSim Contributors. All Rights Reserved.

#include "Vegetation/VegetationCellActor.h"

#include "CesiumGlobeAnchorComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"

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
	GlobeAnchor->MoveToLongitudeLatitudeHeight(FVector(CentreLonDeg, CentreLatDeg, BaseHeightM));
	GlobeAnchor->SetEastSouthUpRotation(FQuat::Identity);
	Trees->SetCastShadow(bCastShadows);
	const int32 CullCm = FMath::Max(1, FMath::RoundToInt32(CullDistanceM * 100.0f));
	Trees->SetCullDistances(0, CullCm);
	Trees->SetStaticMesh(Mesh);
	Trees->AddInstances(LocalInstances, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false, /*bUpdateNavigation=*/false);
}

int32 ACamSimVegetationCell::GetInstanceCount() const
{
	return Trees ? Trees->GetInstanceCount() : 0;
}
