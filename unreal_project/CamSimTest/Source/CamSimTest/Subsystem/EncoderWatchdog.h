// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace CamSimWatchdog
{
	/**
	 * Silent stream death: frames were handed to the capture/encode pipeline
	 * since the last check but none were written. Frames deliberately held back
	 * (terrain gate loading after startup or a teleport, sensor off, output
	 * decimation) submit nothing, so they are not a stall.
	 */
	inline bool IsStalled(uint64 PrevWritten, uint64 CurWritten, uint64 PrevSubmitted, uint64 CurSubmitted)
	{
		return CurSubmitted > PrevSubmitted && CurWritten == PrevWritten;
	}
}
