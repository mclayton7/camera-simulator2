// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * FStencilSlotAllocator (ROADMAP 2.7)
 *
 * Custom-depth stencil values 1..255 for ground-truth entities, lowest free first. A released
 * value is held for ReuseDelayFrames so frames still in the readback ring (which map stencil
 * values to the entities in their own snapshot) never see it on a different entity. Game thread.
 */
class FStencilSlotAllocator
{
public:
	static constexpr int32  MaxValue = 255;
	static constexpr uint64 ReuseDelayFrames = 4;

	uint8 Allocate(uint64 Frame)
	{
		for (int32 V = 1; V <= MaxValue; ++V)
		{
			if (!bInUse[V] && Frame >= FreeFrom[V])
			{
				bInUse[V] = true;
				return static_cast<uint8>(V);
			}
		}
		return 0;
	}

	void Release(uint8 Value, uint64 Frame)
	{
		if (Value == 0 || !bInUse[Value]) return;
		bInUse[Value] = false;
		FreeFrom[Value] = Frame + ReuseDelayFrames;
	}

private:
	bool   bInUse[MaxValue + 1] = {};
	uint64 FreeFrom[MaxValue + 1] = {};
};
