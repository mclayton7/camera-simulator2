// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace CamSimNv12
{
	inline int32 NumBytes(int32 W, int32 H) { return W * H * 3 / 2; }

	/** Full-range BGRA from limited-range BT.709 NV12 (snapshots, tests). */
	void ToBgra(const uint8* Nv12, int32 W, int32 H, TArray<FColor>& Out);

	/** De-interleave NV12 into planar YUV420P (encoder input). */
	void SplitToYuv420p(const uint8* Nv12, int32 W, int32 H,
		uint8* Y, int32 YStride, uint8* U, int32 UStride, uint8* V, int32 VStride);
}
