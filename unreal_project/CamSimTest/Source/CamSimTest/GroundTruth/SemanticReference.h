// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SemanticFrameParams.h"

/**
 * CPU reference of InstanceIdCS's WRITE_SEMANTIC classification (ROADMAP 4D): the shader mirrors it expression for
 * expression and CamSim.GPU.GroundTruth.Semantic.MatchesCpu holds them equal. Change both together.
 */
namespace CamSimSemanticRef
{
	/** The nearest land-cover texel under translated-world point Pw (cm, camera-relative); false outside or non-finite. */
	CAMSIMTEST_API bool NearestLandCoverTexel(const FSemanticFrameParams& P, const FVector3f& Pw, int32& OutX, int32& OutY);

	/**
	 * Class of one source texel: sky (scene device Z <= 0), a visible entity with a class, the sea band on a 0/80 code,
	 * else the land-cover code's class (0 without a window or outside it). LandCoverCode(x, y) reads the window (row 0 =
	 * north). Pw: the scene surface at the texel, translated world cm.
	 */
	CAMSIMTEST_API uint8 ClassifyTexel(const FSemanticFrameParams& P, float SceneZ, float CustomZ, uint32 Stencil,
		const FVector3f& Pw, TFunctionRef<uint8(int32 X, int32 Y)> LandCoverCode);
}
