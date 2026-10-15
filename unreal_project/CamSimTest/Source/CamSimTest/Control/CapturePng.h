// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace CamSimControl
{
	/**
	 * The sensor's NV12 output (BT.709 limited, as encoded) as an 8-bit PNG: what /snapshot serves, for a control
	 * API capture (ROADMAP 2.4). Empty on a size mismatch or failure. Any thread once ImageWrapper is loaded.
	 */
	CAMSIMTEST_API TArray64<uint8> EncodeNv12Png(const TArray<uint8>& Nv12, int32 Width, int32 Height);
}
