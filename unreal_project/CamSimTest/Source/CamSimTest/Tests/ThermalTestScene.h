// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ThermalFrameParams.h"
#include "Thermal/BandRadiance.h"
#include "Thermal/ThermalReference.h"

#include <limits>

/** Synthetic thermal scenes for CamSim.Thermal.Reference.* and CamSim.GPU.Thermal.* (ROADMAP 4A). Test code only. */
namespace CamSimThermalTest
{
	inline constexpr float NearCm  = 10.0f;
	inline constexpr float HFovDeg = 60.0f;

	inline const FBandRadiance& MwirBand()
	{
		static const FBandRadiance Band = [] { FBandRadiance B; B.Build(3.0, 5.0); return B; }();
		return Band;
	}

	/** Three classes (terrain 0, water 1, vehicle 2), every stencil -> vehicle + 8 K, camera 10 m above a flat sea. */
	inline FThermalFrameParams MakeParams(const FRotator& ViewRot, int32 W, int32 H)
	{
		FThermalFrameParams P;
		FMemory::Memcpy(P.LogLut, MwirBand().GetLogLut(), sizeof(P.LogLut));
		P.NumClasses = 3;
		P.ClassTempK[0] = 300.0f; P.ClassEmissivity[0] = 0.95f; P.ClassKFast[0] = 0.015f; P.ClassSAbsRef[0] = 400.0f;   // terrain
		P.ClassTempK[1] = 288.0f; P.ClassEmissivity[1] = 0.98f; P.ClassKFast[1] = 0.0f;   P.ClassSAbsRef[1] = 470.0f;   // water
		P.ClassTempK[2] = 295.0f; P.ClassEmissivity[2] = 0.90f; P.ClassKFast[2] = 0.04f;  P.ClassSAbsRef[2] = 350.0f;   // vehicle
		P.TerrainClass = 0;
		P.WaterClass = 1;
		for (int32 S = 0; S < FThermalFrameParams::NumStencils; ++S) { P.StencilClass[S] = 2; P.StencilOffsetK[S] = 8.0f; }
		P.ClipToTranslatedWorld = CamSimThermalRef::MakeClipToTranslatedWorld(ViewRot, HFovDeg, W, H, NearCm);
		P.Up = FVector3f(0.0f, 0.0f, 1.0f);
		P.bWater = 1;
		P.CamHeightCm = 1000.0f;
		P.SeaRadiusCm = 6.371e8f;
		P.WaterBandCm = 90.0f;
		P.TairK = 288.15f;
		P.SkyEpsZ = 0.7709f;
		P.Cloud = 0.0f;
		P.SkyHemiRadiance = MwirBand().Radiance(276.0f);
		P.BetaPerCm = 0.15f / 1e5f;
		P.KLum = 120.0f;
		P.EClampWm2 = 1200.0f;
		P.KFastScale = 1.0f;
		return P;
	}

	/** Device Z (reversed, infinite far) of the plane z = PlaneZCm (< 0, translated world, camera at the origin) seen through
	 *  output pixel centre (U, V); 0 when the ray misses it (sky). */
	inline float DeviceZForPlane(const FThermalFrameParams& P, const FRotator& ViewRot, float U, float V, float PlaneZCm)
	{
		const FVector4f H = P.ClipToTranslatedWorld.TransformFVector4(FVector4f(U * 2.0f - 1.0f, 1.0f - V * 2.0f, 1.0f, 1.0f));
		const FVector3f Dir = (FVector3f(H.X, H.Y, H.Z) / H.W).GetSafeNormal();
		if (!(Dir.Z < -1e-6f) || !(PlaneZCm < 0.0f)) return 0.0f;
		const float T = PlaneZCm / Dir.Z;
		const float ViewDepth = FVector3f::DotProduct(Dir * T, FVector3f(ViewRot.Vector()));
		return NearCm / ViewDepth;
	}

	/** Scene luminance of an unshadowed horizontal surface of albedo A receiving exactly the terrain class's reference flux:
	 *  (1 - A) E = S_abs,ref, Lum = A E K_lum / pi. */
	inline float SunlitLuminance(const FThermalFrameParams& P, float A)
	{
		return A * P.KLum * (P.ClassSAbsRef[0] / (1.0f - A)) / UE_PI;
	}

	inline bool InBox(float U, float V, float U0, float U1, float V0, float V1) { return U >= U0 && U < U1 && V >= V0 && V < V1; }

	struct FThermalTestScene
	{
		int32 W = 0, H = 0, DW = 0, DH = 0;
		FRotator ViewRot = FRotator(-5.0, 0.0, 0.0);
		FThermalFrameParams P;
		TArray<FLinearColor> Color;    // W x H, absolute luminance units
		TArray<float> Depth, Custom;   // DW x DH, device Z
		TArray<uint8> Stencil;         // DW x DH
		TArray<FLinearColor> Base;     // DW x DH, linear base colour

		CamSimThermalRef::FImages Images(bool bWithBase) const
		{
			CamSimThermalRef::FImages I;
			I.W = W; I.H = H; I.DepthW = DW; I.DepthH = DH;
			I.SceneColor = &Color; I.SceneDepth = &Depth; I.CustomDepth = &Custom; I.Stencil = &Stencil;
			I.BaseColor = bWithBase ? &Base : nullptr;
			return I;
		}
	};

	/**
	 * Camera 10 m above the sea, pitched -5 deg: sky above the horizon; below it a sea-level plane on the left half (water)
	 * and a plane 3 m above the sea on the right (terrain). Entities: visible on terrain (stencil 3, darker base colour),
	 * occluded (4: custom depth twice as far as the scene) and visible on the water (5). A shadowed patch (1/4 of the sunlit
	 * luminance); one NaN and one -Inf colour pixel; one NaN and one +Inf depth texel.
	 */
	inline FThermalTestScene MakeScene(int32 W, int32 H, int32 DW, int32 DH)
	{
		FThermalTestScene S;
		S.W = W; S.H = H; S.DW = DW; S.DH = DH;
		S.P = MakeParams(S.ViewRot, W, H);
		S.Depth.Init(0.0f, DW * DH);
		S.Custom.Init(0.0f, DW * DH);
		S.Stencil.Init(0, DW * DH);
		S.Base.Init(FLinearColor(0.2f, 0.2f, 0.2f, 1.0f), DW * DH);
		for (int32 Ty = 0; Ty < DH; ++Ty)
		{
			for (int32 Tx = 0; Tx < DW; ++Tx)
			{
				const int32 I = Ty * DW + Tx;
				const float U = (Tx + 0.5f) / DW, V = (Ty + 0.5f) / DH;
				const float Z = DeviceZForPlane(S.P, S.ViewRot, U, V, U < 0.5f ? -1000.0f : -700.0f);
				S.Depth[I] = Z;
				if (Z <= 0.0f) continue;
				if (InBox(U, V, 0.60f, 0.70f, 0.70f, 0.80f)) { S.Stencil[I] = 3; S.Custom[I] = Z; S.Base[I] = FLinearColor(0.1f, 0.1f, 0.1f, 1.0f); }
				if (InBox(U, V, 0.80f, 0.90f, 0.70f, 0.80f)) { S.Stencil[I] = 4; S.Custom[I] = Z * 0.5f; }
				if (InBox(U, V, 0.20f, 0.30f, 0.75f, 0.85f)) { S.Stencil[I] = 5; S.Custom[I] = Z; }
			}
		}
		S.Depth[(DH * 9 / 10) * DW + DW * 55 / 100] = std::numeric_limits<float>::quiet_NaN();
		S.Depth[(DH * 9 / 10) * DW + DW * 45 / 100] = std::numeric_limits<float>::infinity();
		const float Lit = SunlitLuminance(S.P, 0.2f);
		S.Color.Init(FLinearColor(Lit, Lit, Lit, 1.0f), W * H);
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				if (InBox((X + 0.5f) / W, (Y + 0.5f) / H, 0.60f, 0.75f, 0.55f, 0.65f))
				{
					S.Color[Y * W + X] = FLinearColor(0.25f * Lit, 0.25f * Lit, 0.25f * Lit, 1.0f);
				}
			}
		}
		S.Color[(H * 95 / 100) * W + W * 65 / 100] = FLinearColor(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 1.0f);
		S.Color[(H * 95 / 100) * W + W * 35 / 100] = FLinearColor(-std::numeric_limits<float>::infinity(), 0.0f, 0.0f, 1.0f);
		return S;
	}
}
