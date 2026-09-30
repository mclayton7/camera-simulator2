// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Ocean/OceanMeshBuilder.h"

class UProceduralMeshComponent;
class UMaterialInterface;
class AActor;

/** The drawn sea: one UProceduralMeshComponent holding the warped grid (Task 7). */
class FOceanMesh
{
public:
	void Init(AActor* Owner, UMaterialInterface* Material);
	/** Replace the geometry; the component moves to Data.OriginWorld. */
	void Upload(const CamSimOcean::FOceanMeshData& Data);
	void SetWorldLocation(const FVector& World);
	FVector GetWorldLocation() const;
	bool IsValid() const;
	/**
	 * Grow the render bounds so vertices displaced by up to PadM metres
	 * (the material's WPO) are not culled. The grid is nearly flat, so its
	 * thin axis needs the most room; the bounds scale is sized for that axis.
	 */
	void SetDisplacementPadding(double PadM);

private:
	TWeakObjectPtr<UProceduralMeshComponent> Component;
	FVector LocalHalfExtent = FVector::ZeroVector;   // cm, of the uploaded grid
	double PadM = 0.0;
	void ApplyBoundsScale();
};
