// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/ScopeLock.h"
#include "SensorFrameParams.h"

/** Newest histogram, render thread → game thread. Older unread ones are overwritten. */
class FSensorStatsMailbox
{
public:
	void Publish(const FSensorHistogram& H) { FScopeLock L(&Lock); Latest = H; bFresh = true; }
	bool TakeLatest(FSensorHistogram& Out) { FScopeLock L(&Lock); if (!bFresh) return false; Out = Latest; bFresh = false; return true; }
private:
	FCriticalSection Lock;
	FSensorHistogram Latest;
	bool bFresh = false;
};
