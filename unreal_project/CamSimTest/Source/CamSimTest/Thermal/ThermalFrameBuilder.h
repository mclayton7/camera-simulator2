// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Config/CamSimConfig.h"
#include "Thermal/LandCoverClasses.h"
#include "ThermalFrameParams.h"
#include "Thermal/BandRadiance.h"
#include "Thermal/ThermalMaterials.h"
#include "Thermal/ThermalModel.h"

/** One stencil-tagged entity, as the entity manager reports it (ROADMAP 4A). */
struct FThermalStencilEntity
{
	uint8   Stencil = 0;              // custom-depth stencil 1..255
	FString ThermalMaterial;          // entity_types.<id>.thermal_material; empty = vehicle_paint
	TOptional<float> ThermalOffsetK;  // entity_types.<id>.thermal_offset_k; unset = +8 K for a surface vehicle, else 0
	bool    bSurfaceVehicle = false;  // land/sea vehicle (placed on the surface)
};

/** The land-cover window one frame maps against (ROADMAP 4B); CamSimThermal::SetLandCover fills it from FLandCoverWindow. */
struct FThermalLandCoverInput
{
	bool    bValid = false;
	uint32  WindowId = 0;                              // FLandCoverWindowData::Id (pairs the params with the GPU window)
	double  CentreLatDeg = 0.0;
	double  CentreLonDeg = 0.0;
	int32   Texels = 0;
	float   TexelM = 10.0f;
	FVector EastWorld  = FVector(1.0, 0.0, 0.0);       // unit East at the window centre, UE world (= translated world direction)
	FVector NorthWorld = FVector(0.0, -1.0, 0.0);
};

/** The sim state one frame of thermal parameters is built from (CamSimThermal::GatherFrameInputs fills it). */
struct FThermalFrameInputs
{
	uint64  UtcMicros  = 0;              // FSimClock::NowMicros
	double  CamLatDeg  = 0.0;
	double  CamLonDeg  = 0.0;
	double  CamAltHaeM = 0.0;
	double  AirTempC     = 15.0;         // daily mean (CIGI Atmosphere Control / thermal.air_temperature_c)
	double  CloudCover01 = 0.0;
	double  VisibilityM  = 10000.0;
	bool    bFogActive   = false;        // CIGI Atmosphere Control fog enabled
	double  WaterTempC   = 15.0;
	bool    bHasSea      = false;        // FOceanSurface present with a sea level here
	double  SeaLevelHaeM = 0.0;          // still sea level (EGM96 + tide), WGS-84 m
	double  MaxWaveAmplitudeM = 0.0;     // sum of the active waves' amplitudes
	FVector UpWorld = FVector::UpVector; // geodetic up at the camera, UE world (= translated world direction)
	double  SunIlluminanceLux = 0.0;     // atmosphere sun light at the ground, on a surface facing it; 0 = none found
	bool    bBaseColorAvailable = true;  // CamSimThermalPass::bBaseColorAtTonemapper
	bool    bBaseColorSrgb = false;      // CamSimThermalPass::bBaseColorSrgbEncoded
	TArray<FThermalStencilEntity> Entities;
	FThermalLandCoverInput LandCover;                  // ROADMAP 4B; invalid = land cover off this frame

};

/**
 * Fills FThermalFrameParams each frame (ROADMAP 4A): LUT for the preset's band, class temperatures from FThermalModel
 * at the local solar time, sky and path terms, K_lum, sea geometry, and the 256-entry stencil table rebuilt from the
 * live entities; land-cover tables, the window mapping and the geo-anchored warp (ROADMAP 4B). ClipToTranslatedWorld is left for the
 * render thread. Game thread.
 */
class CAMSIMTEST_API FThermalFrameBuilder
{
public:
	static constexpr float  DefaultVehicleOffsetK = 8.0f;
	static constexpr double FogVisibilityK  = 3.912;   // Koschmieder: beta = 3.912 / V
	static constexpr double WaterBandBaseM  = 0.5;
	static constexpr double CloudQuantum    = 0.01;    // input quantisation (keeps FThermalModel's exact-compare cache from refitting on jitter)
	static constexpr double AirQuantumK     = 0.05;
	static constexpr double MwirMaxCentreUm = 6.5;     // band centre below this: MWIR extinction
	static constexpr float  AsphaltRampLuma = 0.04f;   // built-up asphalt -> concrete ramp width (base luminance)
	static constexpr double MaxWarpAnchorM  = 200000.0;   // a window farther than this from the warp anchor re-latches it
	static constexpr float  MaxVegBlurM = 32.0f;
	static constexpr float  MaxWarpAmpM = 20.0f, MinWarpCellM = 5.0f, MaxWarpCellM = 200.0f, DefaultWarpCellM = 20.0f;
	const FLandCoverClassTable& GetLandCoverTable() const { return LandCoverTable; }

	/** Rebuild the LUT when the band changes and the material table when thermal.materials changes. */
	void Configure(const FCamSimConfig::FThermalConfig& Cfg, float BandLoUm, float BandHiUm);
	/** This frame's parameters. New warnings (each at most once per builder) are appended to OutWarnings. */
	void Build(const FThermalFrameInputs& In, FThermalFrameParams& Out, TArray<FString>* OutWarnings = nullptr);

	/** Detector signal scale: L / B(300 K) (~1 for a 300 K scene). */
	float GetSignalScale() const { return 1.0f / Band.Radiance(300.0f); }
	bool  IsMwir() const { return 0.5 * (Band.GetLoUm() + Band.GetHiUm()) < MwirMaxCentreUm; }
	const FBandRadiance& GetBand() const { return Band; }
	const FThermalMaterialTable& GetMaterials() const { return Materials; }
	const FThermalModel& GetModel() const { return Model; }

	static double LocalSolarSeconds(uint64 UtcMicros, double LonDeg);
	static FDateTime LocalSolarDate(uint64 UtcMicros, double LonDeg);
	static double GaussianRadiusM(double LatDeg);

private:
	FCamSimConfig::FThermalConfig Config;
	bool                  bConfigured = false;
	FBandRadiance         Band;
	FThermalMaterialTable Materials;
	FThermalModel         Model;
	FLandCoverClassTable  LandCoverTable;
	TArray<FString>       PendingWarnings;   // material table errors, reported by the next Build
	TSet<FString>         WarnedMaterials;
	// Land-cover warp anchor (Task 13): the session's first valid window centre; reset when thermal.land_cover.dir changes.
	bool                  bWarpAnchor = false;
	double                WarpAnchorLatDeg = 0.0;
	double                WarpAnchorLonDeg = 0.0;
};
