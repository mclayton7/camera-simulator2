// Copyright CamSim Contributors. All Rights Reserved.

#include "Ocean/OceanMesh.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "GameFramework/Actor.h"

void FOceanMesh::Init(AActor* Owner, UMaterialInterface* Material)
{
	if (!Owner) return;
	UProceduralMeshComponent* C = NewObject<UProceduralMeshComponent>(Owner, TEXT("OceanMesh"));
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->SetCastShadow(false);
	C->bUseAsyncCooking = false;
	C->SetMobility(EComponentMobility::Movable);
	// M_Ocean treats local axes as UE world axes at 1 cm/unit: the component
	// must never inherit the owner's rotation or scale.
	C->SetUsingAbsoluteLocation(true);
	C->SetUsingAbsoluteRotation(true);
	C->SetUsingAbsoluteScale(true);
	C->SetMaterial(0, Material);
	C->RegisterComponent();
	C->SetWorldTransform(FTransform::Identity);
	C->SetBoundsScale(1.0f);
	Component = C;
}

void FOceanMesh::Upload(const CamSimOcean::FOceanMeshData& Data)
{
	UProceduralMeshComponent* C = Component.Get();
	if (!C) return;
	// UV0 unused; UV1.x = local cell size (m) for M_Ocean. The component stores
	// positions as floats: at 400 km from the origin that rounds by ~2-4 cm,
	// invisible at the horizon.
	TArray<FVector2D> UV0; UV0.Init(FVector2D::ZeroVector, Data.Positions.Num());
	C->CreateMeshSection(0, Data.Positions, Data.Triangles, Data.Normals, UV0, Data.CellSize,
		TArray<FVector2D>(), TArray<FVector2D>(), TArray<FColor>(), TArray<FProcMeshTangent>(), false);
	FBox Box(ForceInit);
	for (const FVector& P : Data.Positions) Box += P;
	LocalHalfExtent = Box.IsValid ? Box.GetExtent() : FVector::ZeroVector;
	SetWorldLocation(Data.OriginWorld);
	ApplyBoundsScale();
}

void FOceanMesh::SetDisplacementPadding(double InPadM)
{
	if (FMath::IsNearlyEqual(InPadM, PadM, 0.01 * FMath::Max(PadM, 1.0))) return;
	PadM = InPadM;
	ApplyBoundsScale();
}

void FOceanMesh::ApplyBoundsScale()
{
	UProceduralMeshComponent* C = Component.Get();
	if (!C || LocalHalfExtent.IsNearlyZero()) return;
	// Scale about the box centre so the thinnest half-extent gains PadM (cm).
	const double Thin = FMath::Max(LocalHalfExtent.GetMin(), 1.0);
	C->SetBoundsScale(static_cast<float>(1.0 + PadM * 100.0 / Thin));
}

void FOceanMesh::SetWorldLocation(const FVector& World)
{
	if (UProceduralMeshComponent* C = Component.Get())
	{
		C->SetWorldLocationAndRotation(World, FQuat::Identity);
	}
}

FVector FOceanMesh::GetWorldLocation() const
{
	const UProceduralMeshComponent* C = Component.Get();
	return C ? C->GetComponentLocation() : FVector::ZeroVector;
}

bool FOceanMesh::IsValid() const
{
	return Component.IsValid();
}
