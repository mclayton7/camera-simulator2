// Copyright CamSim Contributors. All Rights Reserved.

#include "GroundTruth/SemanticFrame.h"
#include "GroundTruth/SemanticClasses.h"
#include "Thermal/LandCoverGeometry.h"
#include "Thermal/LandCoverWindow.h"
#include "Thermal/ThermalFrameBuilder.h"

FSemanticFrameParams CamSimSemantic::BuildFrameParams(const FSemanticBuildInputs& In)
{
	FSemanticFrameParams P;
	const TStaticArray<uint8, 256> Lut = BuildCodeLut();
	for (int32 I = 0; I < 256; ++I)
	{
		P.CodeClass[I] = Lut[I];
		P.StencilClass[I] = In.StencilClass[I];
	}

	const FLandCoverWindowData* W = In.Window;
	auto Finite = [](const FVector& V) { return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z); };
	if (W && W->Id != 0u && W->NonZeroTexels > 0 && W->Spec.Texels >= 2 && W->Spec.TexelM > 0.0
		&& CamSimLandCover::IsWindowAllowed(W->Spec.CentreLatDeg) && Finite(In.EastWorld) && Finite(In.NorthWorld)
		&& !In.EastWorld.IsNearlyZero() && !In.NorthWorld.IsNearlyZero())
	{
		const FVector2D Off = CamSimLandCover::GeodeticToWindowEN(W->Spec, In.CamLatDeg, In.CamLonDeg);   // doubles
		P.bLandCover          = 1u;
		P.LandCoverWindowId   = W->Id;
		P.LandCoverEast       = FVector3f(In.EastWorld.GetSafeNormal());
		P.LandCoverNorth      = FVector3f(In.NorthWorld.GetSafeNormal());
		P.LandCoverCamOffsetM = FVector2f(static_cast<float>(Off.X), static_cast<float>(Off.Y));
		P.LandCoverTexelM     = static_cast<float>(W->Spec.TexelM);
		P.LandCoverTexels     = static_cast<uint32>(W->Spec.Texels);
	}

	// The sea band exactly as FThermalFrameBuilder fills it for ThermalCS
	P.Up = FVector3f(In.UpWorld.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector));
	P.SeaRadiusCm = static_cast<float>(FThermalFrameBuilder::GaussianRadiusM(In.CamLatDeg) * 100.0);
	if (In.SeaLevelHaeM.IsSet() && FMath::IsFinite(*In.SeaLevelHaeM))
	{
		const double Amp = (FMath::IsFinite(In.MaxWaveAmplitudeM) && In.MaxWaveAmplitudeM > 0.0) ? In.MaxWaveAmplitudeM : 0.0;
		P.bSea        = 1u;
		P.CamHeightCm = static_cast<float>((In.CamAltHaeM - *In.SeaLevelHaeM) * 100.0);
		P.WaterBandCm = static_cast<float>((FThermalFrameBuilder::WaterBandBaseM + Amp) * 100.0);
	}
	return P;
}
