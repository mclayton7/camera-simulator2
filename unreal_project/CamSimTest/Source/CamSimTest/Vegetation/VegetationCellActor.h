// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VegetationCellActor.generated.h"

class UCesiumGlobeAnchorComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UStaticMesh;

/**
 * One vegetation cell (REALISM R4 slice, docs/vegetation.md): a globe anchor at the cell centre, oriented East-South-Up,
 * so the actor's local frame is X = East, Y = South, Z = Up in cm and survives origin shifts; one HISM of trees in that
 * frame. No collision (CIGI and the DIS clamp never see trees), no custom depth.
 */
UCLASS(NotPlaceable, Transient)
class CAMSIMTEST_API ACamSimVegetationCell : public AActor
{
	GENERATED_BODY()

public:
	ACamSimVegetationCell();

	/** Game thread, once, right after SpawnActor. LocalInstances are in the cell's ESU frame (cm). */
	void Init(double CentreLatDeg, double CentreLonDeg, double BaseHeightM, UStaticMesh* Mesh,
		const TArray<FTransform>& LocalInstances, bool bCastShadows, float CullDistanceM);

	int32 GetInstanceCount() const;

private:
	UPROPERTY()
	TObjectPtr<USceneComponent> Root;

	UPROPERTY()
	TObjectPtr<UCesiumGlobeAnchorComponent> GlobeAnchor;

	UPROPERTY()
	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> Trees;
};
