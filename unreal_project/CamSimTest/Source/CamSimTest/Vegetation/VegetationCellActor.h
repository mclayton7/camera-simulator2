// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VegetationCellActor.generated.h"

class UCesiumGlobeAnchorComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UStaticMesh;

/** One HISM of a vegetation actor (REALISM R4): one mesh, its instances (actor-local ESU, cm), shadows and cull. */
struct FVegHismBatch
{
	UStaticMesh* Mesh = nullptr;
	TArray<FTransform> Instances;
	bool  bCastShadows = true;
	float CullDistanceM = 0.0f;   // InstanceEndCullDistance; <= 0 or non-finite = no cut
};

/**
 * One vegetation cell or far block (REALISM R4, docs/vegetation.md): a globe anchor at the centre, oriented East-South-Up,
 * so the actor's local frame is X = East, Y = South, Z = Up in cm and survives origin shifts; one HISM per batch (a mesh
 * with its shadows and cull distance) in that frame. No collision (CIGI and the DIS clamp never see trees), no custom depth.
 */
UCLASS(NotPlaceable, Transient)
class CAMSIMTEST_API ACamSimVegetationCell : public AActor
{
	GENERATED_BODY()

public:
	ACamSimVegetationCell();

	/** Game thread, once, right after SpawnActor. Batches with a null mesh or no instances are skipped; cull distances
	 *  are clamped to what the int32 cm cull distance holds. */
	void Init(double CentreLatDeg, double CentreLonDeg, double BaseHeightM, const TArray<FVegHismBatch>& Batches);
	/** One batch (the R4 slice's form; the probe). A null Mesh leaves the cell empty (logged once). */
	void Init(double CentreLatDeg, double CentreLonDeg, double BaseHeightM, UStaticMesh* Mesh,
		const TArray<FTransform>& LocalInstances, bool bCastShadows, float CullDistanceM);

	int32 GetInstanceCount() const;

private:
	UPROPERTY()
	TObjectPtr<USceneComponent> Root;

	UPROPERTY()
	TObjectPtr<UCesiumGlobeAnchorComponent> GlobeAnchor;

	UPROPERTY()
	TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> Batches;
};
