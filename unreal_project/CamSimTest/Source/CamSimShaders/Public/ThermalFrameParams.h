// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Everything one frame of ThermalCS needs besides its input textures (ROADMAP 4A). Filled on the game thread by
 * FThermalFrameBuilder (CamSimTest/Thermal); ClipToTranslatedWorld is set on the render thread from the view.
 * CamSimThermalRef::EvaluatePixel (CamSimTest/Thermal/ThermalReference.h) documents the per-pixel maths.
 */
struct FThermalFrameParams
{
	// In-band radiance LUT: LogLut[i] = ln B(T_i), T_i = LutMinK + i / LutScale (FBandRadiance::Build).
	static constexpr int32 LutSize     = 1024;
	static constexpr float LutMinK     = 150.0f;
	static constexpr float LutMaxK     = 1000.0f;   // upper range left for 4C exhausts
	static constexpr float LutScale    = static_cast<float>(LutSize - 1) / (LutMaxK - LutMinK);
	static constexpr int32 MaxClasses  = 32;
	static constexpr int32 NumStencils = 256;

	float LogLut[LutSize] = {};

	// Classes (index < NumClasses)
	uint32 NumClasses = 1;
	float  ClassTempK[MaxClasses]      = {};   // T_class(t), K
	float  ClassEmissivity[MaxClasses] = {};
	float  ClassKFast[MaxClasses]      = {};   // K per W m^-2
	float  ClassSAbsRef[MaxClasses]    = {};   // (1 - a) S_clear(t) cloud factor, W m^-2: what the class model assumed
	uint32 TerrainClass = 0;
	uint32 WaterClass   = 1;

	// Entities: custom-stencil value -> class + offset (stencil 0 = untagged)
	uint8 StencilClass[NumStencils]   = {};
	float StencilOffsetK[NumStencils] = {};
	float EntityDepthRatio = 0.99f;            // visible when custom depth >= scene depth * ratio (reversed Z: within 1 %)

	// Entity thermal records (ROADMAP 4C), indexed by stencil, EntityRecordFloat4s float4 each:
	//   0-2   world -> body rows: Pb_i = dot(xyz, Pw) + w (Pw translated world cm, Pb body m: X forward, Y right, Z down);
	//         w is filled by FinalizeEntityRecords on the render thread (the view's translation is only known there)
	//   3     (skin T K, class index, part count 0..MaxEntityParts, valid 0/1); invalid -> 4A's StencilClass / StencilOffsetK
	//   4+3k  (centre xyz m, shape 0 box / 1 ellipsoid)
	//   5+3k  (half extents xyz m, falloff m)
	//   6+3k  (T K, 0, 0, 0)
	// HLSL: EntityRecords / EntityPartTemp in CamSimThermalCommon.ush; CPU: CamSimThermalRef::EntityPartTemp.
	static constexpr int32 MaxEntityParts = 4;
	static constexpr int32 EntityRecordFloat4s = 4 + 3 * MaxEntityParts;
	FVector4f EntityRecords[NumStencils * EntityRecordFloat4s];   // zeroed by the constructor (FVector4f has no zero default)
	FVector3d EntityOriginWorld[NumStencils];                    // UE world cm of each valid record's body origin (CPU only)

	FThermalFrameParams()
	{
		FMemory::Memzero(EntityRecords, sizeof(EntityRecords));
		FMemory::Memzero(EntityOriginWorld, sizeof(EntityOriginWorld));
	}

	// Geometry: translated world (cm), the camera at the origin
	FMatrix44f ClipToTranslatedWorld = FMatrix44f::Identity;   // (NDC x, NDC y, device Z, 1) -> translated world, row vector
	FVector3f  Up = FVector3f(0.0f, 0.0f, 1.0f);               // geodetic up at the camera
	uint32 bWater      = 0;                    // 0: no sea surface (ocean off) -> no water class
	float  CamHeightCm = 0.0f;                 // camera height above the still sea surface at its nadir
	float  SeaRadiusCm = 6.371e8f;             // Gaussian radius of curvature at the camera
	float  WaterBandCm = 50.0f;                // water below this height above the sea (0.5 m + max wave amplitude)

	// Sky and atmosphere
	float TairK           = 288.15f;           // air temperature now
	float SkyEpsZ         = 0.77f;             // clear-sky zenith emissivity (T_clear / T_air)^4
	float Cloud           = 0.0f;
	float SkyHemiRadiance = 0.0f;              // view-factor average in-band sky radiance (reflected by 1 - eps)
	float BetaPerCm       = 0.0f;              // path extinction per cm

	// Solar fast term
	float  KLum           = 1.0f;              // lux per W m^-2: E_pix = pi Lum / (max(BaseLum, 0.03) K_lum)
	float  EClampWm2      = 0.0f;              // E_pix clamp: 1.5 S_clear(t)
	float  KFastScale     = 1.0f;              // 0: fast term off (no base colour / no sun light: Task 1 fallback)
	uint32 bBaseColorSrgb = 0;                 // 1: GBufferC values are sRGB-encoded (Task 1 SRGB_ENCODED)

	// Land cover (ROADMAP 4B). Terrain pixels inside the camera-centred window blend the class data of the four nearest
	// WorldCover texels (refined per pixel by the base colour); outside it, or with bLandCover = 0, they are TerrainClass (4A).
	static constexpr int32 NumLandCoverCodes = 256;
	static constexpr uint8 LandCoverFamilyNone = 0, LandCoverFamilyVegetation = 1, LandCoverFamilyBuiltUp = 2, LandCoverFamilyBare = 3;
	uint8     LandCoverClass[NumLandCoverCodes]  = {};   // WorldCover code -> class index
	uint8     LandCoverFamily[NumLandCoverCodes] = {};   // WorldCover code -> LandCoverFamily*
	uint32    bLandCover          = 0;
	uint32    LandCoverWindowId   = 0;                   // FLandCoverWindowData::Id these mapping values belong to (0 = none)
	// Window coordinates of a translated-world point Pw (cm): E = dot(Pw, LandCoverEast) / 100 + LandCoverCamOffsetM.X,
	// N = dot(Pw, LandCoverNorth) / 100 + LandCoverCamOffsetM.Y (metres; +E east, +N north). East/North are unit,
	// dimensionless, expressed in UE world axes (translated world shares them) at the window centre; the offset comes from
	// CamSimLandCover::GeodeticToWindowEN(camera) (doubles on the CPU), never from a separate tangent-plane transform.
	// Texel (x, y) centre: E = (x + 0.5 - T/2) TexelM, N = (T/2 - y - 0.5) TexelM (row 0 north, column 0 west, row-major).
	FVector3f LandCoverEast       = FVector3f(1.0f, 0.0f, 0.0f);    // unit East at the window centre, UE world axes
	FVector3f LandCoverNorth      = FVector3f(0.0f, -1.0f, 0.0f);   // unit North (UE +Y is south at the georeference origin)
	FVector2f LandCoverCamOffsetM = FVector2f::ZeroVector;          // camera (East, North) from the window centre, m (CPU doubles)
	float     LandCoverTexelM     = 10.0f;
	uint32    LandCoverTexels     = 2048;
	// Geo-anchored domain warp of the lookup (Task 13: breaks the visible 10 m grid). Ground coordinates of window point (E, N):
	// G = LandCoverAnchorM + LandCoverAnchorScale * (E, N), i.e. CamSimLandCover::GeodeticToWindowEN(session anchor, point)
	// exactly (both mappings are linear in lat/lon), so G is fixed to the ground across re-centres. The lookup position is
	// (E, N) + LandCoverWarpAmpM * (n1(G), n2(G)), n1, n2 in [-1, 1]: smoothstep value noise of the PCG hash on a lattice
	// of LandCoverWarpCellM (CamSimHash::LandCoverWarpStream). LandCoverWarpAmpM <= 0: no warp (the struct default).
	FVector2f LandCoverAnchorM     = FVector2f::ZeroVector;   // window centre from the session anchor, m (CPU doubles)
	FVector2f LandCoverAnchorScale = FVector2f(1.0f, 1.0f);   // (N cos phi)_anchor / (N cos phi)_centre, M_anchor / M_centre
	float     LandCoverWarpAmpM    = 0.0f;
	float     LandCoverWarpCellM   = 20.0f;
	uint32    bLandCoverRefine    = 0;                   // base-colour refinement (needs the base colour, not sunlight)
	float     VegIndexLo          = 0.05f;               // v = saturate((ExG - lo) / max(hi - lo, 1e-4))
	float     VegIndexHi          = 0.20f;
	float     AsphaltMaxLuma      = 0.12f;               // built-up concrete weight = saturate((BaseLum - max) / ramp + 0.5)
	float     AsphaltRampLuma     = 0.04f;
	// Vegetation index from a world-space blurred base colour (fix round 1: JPEG chroma blocks in the imagery): the centre texel
	// and 4 diagonal taps (+R, +R), (-R, +R), (+R, -R), (-R, -R) (never on the centre's row or column, so an axis-aligned JPEG block
	// edge flips one tap at a time), R = clamp(floor(VegBlurM * 100 / (Range_cm * BaseTexelAngle)
	// + 0.5), 1, 32). The asphalt/concrete split stays on the centre texel. VegBlurM <= 0 or BaseTexelAngle <= 0: single tap.
	float     VegBlurM            = 0.0f;                // thermal.land_cover.veg_blur_m
	float     BaseTexelAngle      = 0.0f;                // radians per base-colour (render-resolution) texel; set on the render thread
	uint32    VegetationClass     = 4;                   // refinement targets (FThermalMaterialTable indices)
	uint32    BareSoilClass       = 11;
	uint32    AsphaltClass        = 3;
	uint32    ConcreteClass       = 5;

	float InputScale = 1.0f;                   // multiplies scene colour; tests only (runtime also multiplies View.OneOverPreExposure)
};

/**
 * Render thread (and tests): rows 0-2 .w of every valid entity record from its origin in translated world,
 * w_i = -dot(row_i.xyz, origin + PreViewTranslation), in doubles until the dot (precision far from the georeference origin).
 */
inline void FinalizeEntityRecords(FThermalFrameParams& P, const FVector3d& PreViewTranslation)
{
	for (int32 S = 1; S < FThermalFrameParams::NumStencils; ++S)
	{
		FVector4f* R = &P.EntityRecords[S * FThermalFrameParams::EntityRecordFloat4s];
		if (R[3].W == 0.0f) continue;
		const FVector3d O = P.EntityOriginWorld[S] + PreViewTranslation;
		for (int32 I = 0; I < 3; ++I)
		{
			R[I].W = static_cast<float>(-(double(R[I].X) * O.X + double(R[I].Y) * O.Y + double(R[I].Z) * O.Z));
		}
	}
}

/**
 * B(T) from the LUT: T clamped to [LutMinK, LutMaxK] (NaN -> LutMinK), ln B interpolated linearly, then exp.
 * HLSL twin: LutRadiance in Shaders/Private/CamSimThermalCommon.ush (same expressions, same order).
 */
inline float ThermalLutRadiance(const float* LogLut, float TK)
{
	float T = TK;
	if (!(T >= FThermalFrameParams::LutMinK)) T = FThermalFrameParams::LutMinK;
	if (T > FThermalFrameParams::LutMaxK) T = FThermalFrameParams::LutMaxK;
	const float X = (T - FThermalFrameParams::LutMinK) * FThermalFrameParams::LutScale;
	const int32 I = FMath::Min(FMath::FloorToInt32(X), FThermalFrameParams::LutSize - 2);
	const float F = X - static_cast<float>(I);
	const float A = LogLut[I];
	return FMath::Exp(A + (LogLut[I + 1] - A) * F);
}
