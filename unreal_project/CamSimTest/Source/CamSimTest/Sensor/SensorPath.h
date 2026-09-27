// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FCamSimConfig;

enum class ESensorPipelinePath : uint8 { Legacy = 0, Gpu = 1 };

struct FSensorPathDecision
{
	ESensorPipelinePath Path = ESensorPipelinePath::Legacy;
	TArray<FString> Unported;   // enabled effects the GPU path doesn't support yet
	FString Reason;             // one log line
	bool bError = false;        // the GPU path was wanted but can't run: log Reason as an error
};

struct FSensorPathSelector
{
	static FSensorPathDecision Decide(const FCamSimConfig& Cfg);
	static bool WantsGpu(const FCamSimConfig& Cfg);
	static void DowngradeToLegacy(FSensorPathDecision& D, const FString& Why);
	static const TCHAR* ToString(ESensorPipelinePath P) { return P == ESensorPipelinePath::Gpu ? TEXT("gpu") : TEXT("legacy"); }
};
