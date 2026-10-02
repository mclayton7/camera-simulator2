// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ThermalFrameParams.h"

/**
 * CPU reference of ThermalCS (ROADMAP 4A). Shaders/Private/CamSimThermalCommon.ush EvaluatePixel mirrors
 * EvaluatePixel below expression for expression; CamSim.GPU.Thermal.MatchesCpu holds them to 1e-4 relative.
 * Per output pixel (in order):
 *  0. non-finite scene colour or depth -> B(T_air) (Invalid)
 *  1. sky: device Z <= 0 -> B(T_sky(el)), el from the view ray, T_sky = T_air ((1-c) eps(el) + c)^(1/4)
 *  2. entity: stencil s > 0 and custom Z >= scene Z * EntityDepthRatio -> class/offset from the stencil table, or (valid
 *     4C record) the record's class with T = EntityPartTemp instead of T_class + offset
 *  3. water: height above the sea h = H_cam + v + d^2 / 2R < WaterBandCm (v = P.up, d^2 = |P|^2 - v^2)
 *  4. terrain: TerrainClass
 *  4b. terrain inside the land-cover window (ROADMAP 4B): class data = blend of the four nearest texels' refined class data
 *      at the geo-anchored warped position, smoothstep-bilinear weights (SampleLandCover, WarpLandCoverEN, RefinedClassData,
 *      BlendLandCover); outside it (warped position off the texel-centre grid) TerrainClass
 *  T = T_class + offset [+ KFastScale k_fast ((1 - a_pix) E_pix - S_abs,ref)], a_pix = BaseLum,
 *  E_pix = clamp(pi Lum(colour) / (max(BaseLum, 0.03) max(K_lum, 1e-6)), 0, EClamp)
 *  L = tau (eps B(T) + (1 - eps) L_sky,hemi) + (1 - tau) B(T_air), tau = exp(-beta range)
 */
namespace CamSimThermalRef
{
	enum class EPixelClass : uint8 { Sky, Entity, Water, Terrain, Invalid };

	/** What ThermalCS reads for one output pixel. */
	struct FPixelSample
	{
		float U = 0.5f, V = 0.5f;                       // output pixel centre / output size
		float DeviceZ = 0.0f;                           // scene depth, reversed (1 near, 0 far)
		float CustomZ = 0.0f;                           // custom depth of tagged entities
		uint32 Stencil = 0;                             // custom stencil (low 8 bits used)
		FVector3f Color = FVector3f::ZeroVector;        // scene colour * InputScale (absolute luminance units)
		FVector3f Base = FVector3f::ZeroVector;         // GBuffer base colour
		bool bHasBase = false;                          // the base colour texture is bound
		// Vegetation-index taps (fix round 1): raw base colour at the diagonals (+R, +R), (-R, +R), (+R, -R), (-R, -R) texels,
		// R = VegBlurRadiusPx; Run fills them
		// when the base colour is bound and IsVegBlurOn. Without them EvaluatePixel uses the single centre texel.
		FVector3f BaseTaps[4] = { FVector3f::ZeroVector, FVector3f::ZeroVector, FVector3f::ZeroVector, FVector3f::ZeroVector };
		bool bBaseTaps = false;
		const uint8* LandCover = nullptr;               // window codes (P.LandCoverTexels^2, row 0 = north); null: none bound
	};

	struct FPixelResult
	{
		EPixelClass Class = EPixelClass::Terrain;
		float TempK = 0.0f;
		float Radiance = 0.0f;                          // W m^-2 sr^-1 in band
		bool bLandCover = false;                        // terrain whose class data came from land cover
	};

	/** Row-major view-rect contents: colour W x H (output resolution), depth/stencil/base DepthW x DepthH (render resolution). */
	struct FImages
	{
		int32 W = 0, H = 0, DepthW = 0, DepthH = 0;
		const TArray<FLinearColor>* SceneColor  = nullptr;
		const TArray<float>*        SceneDepth  = nullptr;
		const TArray<float>*        CustomDepth = nullptr;
		const TArray<uint8>*        Stencil     = nullptr;
		const TArray<FLinearColor>* BaseColor   = nullptr;   // null: not bound (fast term off)
		const TArray<uint8>*        LandCover   = nullptr;   // P.LandCoverTexels^2 window codes; null: not bound
	};

	/** The four window texels around a (warped) world position and their smoothstep-bilinear weights (ROADMAP 4B). */
	struct FLandCoverSample
	{
		bool  bInside = false;       // false: no window, land cover off, or outside the texel-centre grid -> TerrainClass
		uint8 Codes[4] = {};         // (x0, y0), (x1, y0), (x0, y1), (x1, y1); y0 is the northern row
		float Weights[4] = {};
	};

	float     Lum709(const FVector3f& C);
	FVector3f ClipToWorld(const FThermalFrameParams& P, float Nx, float Ny, float Z);
	/** (T_class, emissivity, k_fast, S_abs,ref) of a class; the index is clamped to NumClasses. */
	FVector4f ClassData(const FThermalFrameParams& P, uint32 Class);
	/** The two warp fields (n1, n2), each in [-1, 1], at ground coordinates G (m): smoothstep-bilinear value noise of
	 *  CamSimHash::Pcg on a lattice of P.LandCoverWarpCellM (stream CamSimHash::LandCoverWarpStream; n1 = high 16 bits,
	 *  n2 = low 16 bits of each corner's hash). C1, deterministic, no floating-point hash. */
	FVector2f LandCoverWarpNoise(const FThermalFrameParams& P, float Gx, float Gy);
	/** Warped window position (Task 13): (E, N) + LandCoverWarpAmpM (n1, n2)(G), G = LandCoverAnchorM + LandCoverAnchorScale (E, N)
	 *  (ground-fixed across re-centres). Identity when LandCoverWarpAmpM <= 0; non-finite G is returned as is (the lookup rejects it). */
	FVector2f WarpLandCoverEN(const FThermalFrameParams& P, float E, float N);
	/** Pw: camera-relative world position (cm). (E, N) = (Pw . East, Pw . North) / 100 + CamOffset, then WarpLandCoverEN; texel
	 *  centres on integers. Weights: bilinear of the smoothstep fractions S(f) = f f (3 - 2 f) (C1 across texel centres). */
	FLandCoverSample SampleLandCover(const FThermalFrameParams& P, const uint8* Codes, const FVector3f& Pw);
	/** (vegetation v, concrete c) of a linear base colour. */
	FVector2f RefinementWeights(const FThermalFrameParams& P, const FVector3f& Base);
	/** v from VegBase (the blurred base colour), c from Base (the full-resolution centre texel). */
	FVector2f RefinementWeights(const FThermalFrameParams& P, const FVector3f& Base, const FVector3f& VegBase);
	/** The vegetation blur is on: VegBlurM > 0 and BaseTexelAngle > 0 (both finite). */
	bool  IsVegBlurOn(const FThermalFrameParams& P);
	/** Tap offset in base-colour texels: clamp(floor(VegBlurM * 100 / max(RangeCm * BaseTexelAngle, 1e-6) + 0.5), 1, 32). */
	int32 VegBlurRadiusPx(const FThermalFrameParams& P, float RangeCm);
	/** One code's class data; with bRefine the family's blend toward vegetation / bare soil / asphalt / concrete. */
	FVector4f RefinedClassData(const FThermalFrameParams& P, uint8 Code, const FVector2f& Weights, bool bRefine);
	/** Bilinear blend of the four texels' refined class data. */
	FVector4f BlendLandCover(const FThermalFrameParams& P, const FLandCoverSample& S, const FVector3f& Base, bool bRefine);
	/** As above, v from VegBase. */
	FVector4f BlendLandCover(const FThermalFrameParams& P, const FLandCoverSample& S, const FVector3f& Base, const FVector3f& VegBase, bool bRefine);

	/**
	 * Entity pixel temperature from a valid 4C record (ROADMAP 4C): Pb = rows 0-2 applied to Pw (translated world cm -> body m);
	 * T = skin; for each part k in order: d = outside distance (box: |max(|Pb - c| - h, 0)|; ellipsoid: (|(Pb - c) / h| - 1) min(h)),
	 * w = 1 - smoothstep(0, falloff, d) (smoothstep as t = saturate(d / falloff), t^2 (3 - 2t)), T += (T_k - T) w.
	 * HLSL twin: EntityPartTemp in CamSimThermalCommon.ush.
	 */
	float EntityPartTemp(const FThermalFrameParams& P, uint32 Stencil, const FVector3f& Pw);
	float LutRadiance(const FThermalFrameParams& P, float TK);
	float SkyTemperatureK(const FThermalFrameParams& P, float SinEl);
	float SrgbToLinear(float C);
	FPixelResult EvaluatePixel(const FThermalFrameParams& P, const FPixelSample& S);
	/** Every output pixel; pixel (x, y) reads texel (floor(U DepthW), floor(V DepthH)) of the depth-resolution images. */
	TArray<FPixelResult> Run(const FImages& In, const FThermalFrameParams& P);
	/** ClipToTranslatedWorld of a camera at the origin with this rotation and horizontal FOV (UE's reversed-Z infinite projection). */
	FMatrix44f MakeClipToTranslatedWorld(const FRotator& ViewRotation, float HFovDeg, int32 W, int32 H, float NearCm);
}
