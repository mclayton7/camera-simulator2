// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GroundTruth/AnnotationTypes.h"

/**
 * Turns the output-space instance-ID image into per-entity mask measurements (pure C++, task thread).
 * See docs/superpowers/specs/2026-09-30-atr-ground-truth-design.md.
 */
struct FInstanceMaskAnalyzer
{
	/** Fills the mask fields of every entity with StencilValue != 0, sets ScreenBBox to the modal box and
	 *  bTruncated, and removes tagged entities with VisiblePixels < MinVisiblePixels. Invalid Ids: no-op. */
	static void Analyze(const FInstanceIdImage& Ids, TArray<FEntityAnnotationData>& Entities,
	                    int32 MinVisiblePixels, bool bSegmentation);
};
