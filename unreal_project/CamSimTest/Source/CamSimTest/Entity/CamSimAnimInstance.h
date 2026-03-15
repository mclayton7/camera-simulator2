// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "CamSimAnimInstance.generated.h"

/**
 * UCamSimAnimInstance
 *
 * Lightweight AnimInstance subclass for CamSim character entities.
 * Exposes GroundSpeed and AnimStateIndex to Animation Blueprints
 * for state-machine driven locomotion blending.
 *
 * AnimStateIndex derivation from GroundSpeed:
 *   0 = Idle   (< 0.1 m/s)
 *   1 = Walk   (< 1.5 m/s)
 *   2 = Run    (>= 1.5 m/s)
 *
 * CIGI Component Control CompId=20 can override stance:
 *   3 = Crouch
 *   4 = Prone
 */
UCLASS()
class CAMSIMTEST_API UCamSimAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadWrite, Category = "CamSim")
	float GroundSpeed = 0.0f;

	// 0=Idle, 1=Walk, 2=Run, 3=Crouch, 4=Prone
	UPROPERTY(BlueprintReadWrite, Category = "CamSim")
	int32 AnimStateIndex = 0;

	// If true, AnimStateIndex was set by CIGI Component Control and auto-derive is suppressed.
	UPROPERTY(BlueprintReadWrite, Category = "CamSim")
	bool bManualState = false;

	virtual void NativeUpdateAnimation(float DeltaSeconds) override;
};
