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
	FVegHismBatch B;
	B.Mesh = Mesh;
	B.Instances = LocalInstances;
	B.bCastShadows = bCastShadows;
	B.CullDistanceM = CullDistanceM;
	Init(CentreLatDeg, CentreLonDeg, BaseHeightM, TArray<FVegHismBatch>{ MoveTemp(B) });
}

void ACamSimVegetationCell::Init(double CentreLatDeg, double CentreLonDeg, double BaseHeightM, const TArray<FVegHismBatch>& InBatches)
{
	GlobeAnchor->MoveToLongitudeLatitudeHeight(FVector(CentreLonDeg, CentreLatDeg, BaseHeightM));
	GlobeAnchor->SetEastSouthUpRotation(FQuat::Identity);
	for (const FVegHismBatch& B : InBatches)
	{
		if (!B.Mesh || B.Instances.Num() == 0) continue;
		UHierarchicalInstancedStaticMeshComponent* H = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
		H->SetupAttachment(Root);
		H->SetMobility(EComponentMobility::Movable);
		H->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		H->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		H->SetCanEverAffectNavigation(false);
		H->SetRenderCustomDepth(false);
		H->SetCastShadow(B.bCastShadows);
		// Clamped in double before the int32 conversion (cull distances come from config); <= 0 or non-finite = no cut.
		const double CullCmD = FMath::IsFinite(B.CullDistanceM) && B.CullDistanceM > 0.0f
			? FMath::Clamp(static_cast<double>(B.CullDistanceM) * 100.0, 1.0, static_cast<double>(MAX_int32)) : static_cast<double>(MAX_int32);
		H->SetCullDistances(0, static_cast<int32>(CullCmD));
		H->SetStaticMesh(B.Mesh);
		H->RegisterComponent();
		H->AddInstances(B.Instances, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false, /*bUpdateNavigation=*/false);
		Batches.Add(H);
	}
}

int32 ACamSimVegetationCell::GetInstanceCount() const
{
	int32 N = 0;
	for (const TObjectPtr<UHierarchicalInstancedStaticMeshComponent>& H : Batches) if (H) N += H->GetInstanceCount();
	return N;
}
