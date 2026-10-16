// Copyright CamSim Contributors. All Rights Reserved.

#include "GroundTruth/SemanticReference.h"
#include "GroundTruth/SemanticClasses.h"

bool CamSimSemanticRef::NearestLandCoverTexel(const FSemanticFrameParams& P, const FVector3f& Pw, int32& OutX, int32& OutY)
{
	OutX = OutY = 0;
	if (P.bLandCover == 0u || P.LandCoverTexels == 0u) return false;
	const float E = FVector3f::DotProduct(Pw, P.LandCoverEast) / 100.0f + P.LandCoverCamOffsetM.X;
	const float N = FVector3f::DotProduct(Pw, P.LandCoverNorth) / 100.0f + P.LandCoverCamOffsetM.Y;
	const float Half = static_cast<float>(P.LandCoverTexels) * 0.5f;
	const float X = E / P.LandCoverTexelM + Half - 0.5f;
	const float Y = Half - N / P.LandCoverTexelM - 0.5f;
	if (!FMath::IsFinite(X) || !FMath::IsFinite(Y)) return false;
	const float Rx = FMath::FloorToFloat(X + 0.5f);
	const float Ry = FMath::FloorToFloat(Y + 0.5f);
	const float MaxI = static_cast<float>(P.LandCoverTexels) - 1.0f;
	if (Rx < 0.0f || Rx > MaxI || Ry < 0.0f || Ry > MaxI) return false;
	OutX = static_cast<int32>(Rx);
	OutY = static_cast<int32>(Ry);
	return true;
}

uint8 CamSimSemanticRef::ClassifyTexel(const FSemanticFrameParams& P, float SceneZ, float CustomZ, uint32 Stencil,
	const FVector3f& Pw, TFunctionRef<uint8(int32 X, int32 Y)> LandCoverCode)
{
	if (!(SceneZ > 0.0f)) return CamSimSemantic::Sky;
	const uint32 S = Stencil & 0xFFu;
	if (S != 0u && CustomZ >= SceneZ * (1.0f - 1e-4f) && P.StencilClass[S] != 0u) return P.StencilClass[S];

	int32 X = 0, Y = 0;
	const uint32 Code = NearestLandCoverTexel(P, Pw, X, Y) ? (LandCoverCode(X, Y) & 0xFFu) : 0u;
	if (P.bSea != 0u && (Code == 0u || Code == 80u))
	{
		const float Vert = FVector3f::DotProduct(Pw, P.Up);
		const float Range2 = FVector3f::DotProduct(Pw, Pw);
		const float D2 = FMath::Max(Range2 - Vert * Vert, 0.0f);
		const float Height = P.CamHeightCm + Vert + D2 / (2.0f * P.SeaRadiusCm);
		if (Height < P.WaterBandCm) return CamSimSemantic::Water;
	}
	return P.CodeClass[Code];
}
