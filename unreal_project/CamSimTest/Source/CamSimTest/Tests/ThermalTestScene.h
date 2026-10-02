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
		TArray<uint8> LandCover;       // P.LandCoverTexels^2 window codes (ROADMAP 4B); empty: none bound

		CamSimThermalRef::FImages Images(bool bWithBase, bool bWithLandCover = true) const
		{
			CamSimThermalRef::FImages I;
			I.W = W; I.H = H; I.DepthW = DW; I.DepthH = DH;
			I.SceneColor = &Color; I.SceneDepth = &Depth; I.CustomDepth = &Custom; I.Stencil = &Stencil;
			I.BaseColor = bWithBase ? &Base : nullptr;
			I.LandCover = (bWithLandCover && LandCover.Num() > 0) ? &LandCover : nullptr;
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

	// ---- ROADMAP 4B land cover ----
	inline constexpr uint32 LcAsphalt = 3, LcVegetation = 4, LcConcrete = 5, LcTree = 6, LcBare = 7, LcBuiltUp = 8;

	/**
	 * MakeParams without the sea, plus land classes 3..8 with distinct values and a Texels^2 window of TexelM-metre texels whose
	 * axes are rotated YawDeg about +Z. Codes: 10 -> tree, 30 -> vegetation (both vegetation family), 50 -> built-up,
	 * 60 -> bare, 80 -> water (no family), everything else -> terrain.
	 */
	inline FThermalFrameParams MakeLandCoverParams(const FRotator& ViewRot, int32 W, int32 H, uint32 Texels, float TexelM, float YawDeg)
	{
		FThermalFrameParams P = MakeParams(ViewRot, W, H);
		P.bWater = 0;
		P.NumClasses = 9;
		auto Set = [&P](uint32 C, float T, float Eps, float K, float S) { P.ClassTempK[C] = T; P.ClassEmissivity[C] = Eps; P.ClassKFast[C] = K; P.ClassSAbsRef[C] = S; };
		Set(LcAsphalt,    310.0f, 0.95f, 0.020f, 420.0f);
		Set(LcVegetation, 296.0f, 0.98f, 0.008f, 380.0f);
		Set(LcConcrete,   304.0f, 0.92f, 0.015f, 300.0f);
		Set(LcTree,       293.0f, 0.98f, 0.004f, 280.0f);
		Set(LcBare,       312.0f, 0.93f, 0.025f, 350.0f);
		Set(LcBuiltUp,    307.0f, 0.93f, 0.018f, 380.0f);
		for (int32 C = 0; C < FThermalFrameParams::NumLandCoverCodes; ++C) { P.LandCoverClass[C] = 0; P.LandCoverFamily[C] = FThermalFrameParams::LandCoverFamilyNone; }
		P.LandCoverClass[10] = LcTree;       P.LandCoverFamily[10] = FThermalFrameParams::LandCoverFamilyVegetation;
		P.LandCoverClass[30] = LcVegetation; P.LandCoverFamily[30] = FThermalFrameParams::LandCoverFamilyVegetation;
		P.LandCoverClass[50] = LcBuiltUp;    P.LandCoverFamily[50] = FThermalFrameParams::LandCoverFamilyBuiltUp;
		P.LandCoverClass[60] = LcBare;       P.LandCoverFamily[60] = FThermalFrameParams::LandCoverFamilyBare;
		P.LandCoverClass[80] = 1;            // water class
		P.VegetationClass = LcVegetation; P.BareSoilClass = LcBare; P.AsphaltClass = LcAsphalt; P.ConcreteClass = LcConcrete;
		P.bLandCover = 1;
		P.LandCoverWindowId = 7;
		const float Yaw = FMath::DegreesToRadians(YawDeg);
		P.LandCoverEast  = FVector3f(FMath::Cos(Yaw), FMath::Sin(Yaw), 0.0f);
		P.LandCoverNorth = FVector3f(FMath::Sin(Yaw), -FMath::Cos(Yaw), 0.0f);   // UE +Y is south at the georeference origin
		P.LandCoverCamOffsetM = FVector2f(1.7f, -2.3f);
		P.LandCoverTexelM = TexelM;
		P.LandCoverTexels = Texels;
		P.bLandCoverRefine = 1;
		return P;
	}

	/** Quadrants: north-west 10, north-east 50, south-west 60, south-east 30; then row 0 (north edge) = 80, column 0 (west edge) = 0. */
	inline TArray<uint8> MakeLandCoverCodes(uint32 Texels)
	{
		TArray<uint8> C;
		C.SetNumUninitialized(Texels * Texels);
		const uint32 Half = Texels / 2;
		for (uint32 Y = 0; Y < Texels; ++Y)
		{
			for (uint32 X = 0; X < Texels; ++X)
			{
				uint8 V = Y < Half ? (X < Half ? 10 : 50) : (X < Half ? 60 : 30);
				if (Y == 0) V = 80;
				if (X == 0) V = 0;
				C[Y * Texels + X] = V;
			}
		}
		return C;
	}

	/** Camera-relative (translated) world position, cm, of window coordinates (EastM, NorthM) at height ZCm. */
	inline FVector3f GroundPoint(const FThermalFrameParams& P, float EastM, float NorthM, float ZCm)
	{
		return P.LandCoverEast * ((EastM - P.LandCoverCamOffsetM.X) * 100.0f) + P.LandCoverNorth * ((NorthM - P.LandCoverCamOffsetM.Y) * 100.0f)
			+ FVector3f(0.0f, 0.0f, ZCm);
	}

	/**
	 * Camera 7 m above flat terrain, pitched -50 deg. A 32^2 window of 0.4 m texels (MakeLandCoverCodes) whose centre is in view,
	 * its far edge crossed by the footprint (outside pixels exist). Base colour by U: green, green->grey ramp (intermediate v),
	 * dark grey (asphalt), bright grey (concrete). One visible entity (stencil 3). Sunlit colour (fast term active).
	 */
	inline FThermalTestScene MakeLandCoverScene(int32 W, int32 H, int32 DW, int32 DH, float YawDeg)
	{
		FThermalTestScene S;
		S.W = W; S.H = H; S.DW = DW; S.DH = DH;
		S.ViewRot = FRotator(-50.0, 0.0, 0.0);
		S.P = MakeLandCoverParams(S.ViewRot, W, H, 32, 0.4f, YawDeg);
		S.P.LandCoverCamOffsetM = FVector2f(-4.5f, 0.3f);
		S.LandCover = MakeLandCoverCodes(32);
		S.Depth.Init(0.0f, DW * DH);
		S.Custom.Init(0.0f, DW * DH);
		S.Stencil.Init(0, DW * DH);
		S.Base.Init(FLinearColor(0.2f, 0.2f, 0.2f, 1.0f), DW * DH);
		const FLinearColor Green(0.05f, 0.20f, 0.04f, 1.0f), Grey(0.2f, 0.2f, 0.2f, 1.0f);
		for (int32 Ty = 0; Ty < DH; ++Ty)
		{
			for (int32 Tx = 0; Tx < DW; ++Tx)
			{
				const int32 I = Ty * DW + Tx;
				const float U = (Tx + 0.5f) / DW, V = (Ty + 0.5f) / DH;
				const float Z = DeviceZForPlane(S.P, S.ViewRot, U, V, -700.0f);
				S.Depth[I] = Z;
				if (Z <= 0.0f) continue;
				if (U < 0.25f)      S.Base[I] = Green;
				else if (U < 0.5f)  S.Base[I] = FMath::Lerp(Green, Grey, V);
				else if (U < 0.75f) S.Base[I] = FLinearColor(0.06f, 0.06f, 0.06f, 1.0f);
				else                S.Base[I] = FLinearColor(0.40f, 0.40f, 0.42f, 1.0f);
				if (InBox(U, V, 0.45f, 0.55f, 0.80f, 0.90f)) { S.Stencil[I] = 3; S.Custom[I] = Z; }
			}
		}
		const float Lit = SunlitLuminance(S.P, 0.2f);
		S.Color.Init(FLinearColor(Lit, Lit, Lit, 1.0f), W * H);
		S.P.BaseTexelAngle = 2.0f * FMath::Tan(FMath::DegreesToRadians(0.5f * HFovDeg)) / static_cast<float>(DW);
		return S;
	}

	/**
	 * Fix round 1 (JPEG chroma blocks): replace the scene's ground base colour by a chroma checker of BlockPx-texel squares,
	 * a muted green (ExG 0.22: v = 1, just past veg_index_hi, like real imagery chroma) against a grey of the same luminance
	 * (v = 0); the luminance (asphalt/concrete split) is uniform.
	 */
	inline void ApplyChromaChecker(FThermalTestScene& S, int32 BlockPx)
	{
		const FLinearColor Green(0.15f, 0.20f, 0.14f, 1.0f);
		const float L = 0.2126f * Green.R + 0.7152f * Green.G + 0.0722f * Green.B;
		const FLinearColor Grey(L, L, L, 1.0f);
		for (int32 Ty = 0; Ty < S.DH; ++Ty)
		{
			for (int32 Tx = 0; Tx < S.DW; ++Tx)
			{
				S.Base[Ty * S.DW + Tx] = (((Tx / BlockPx) + (Ty / BlockPx)) % 2 == 0) ? Green : Grey;
			}
		}
	}
}
