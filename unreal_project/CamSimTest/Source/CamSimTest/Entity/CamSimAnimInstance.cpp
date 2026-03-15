// Copyright CamSim Contributors. All Rights Reserved.

#include "Entity/CamSimAnimInstance.h"

void UCamSimAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);

	// Auto-derive locomotion state from ground speed unless manually overridden
	if (!bManualState)
	{
		if (GroundSpeed < 0.1f)
			AnimStateIndex = 0; // Idle
		else if (GroundSpeed < 1.5f)
			AnimStateIndex = 1; // Walk
		else
			AnimStateIndex = 2; // Run
	}
}
