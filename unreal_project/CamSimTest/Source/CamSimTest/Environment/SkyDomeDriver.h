// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Config/CamSimConfig.h"
#include "Environment/MoonSurface.h"
#include "SkyDomeDriver.generated.h"

class AActor;
class UStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialParameterCollection;
class UTexture2D;
struct FEphemerisResult;
struct FNightIlluminationResult;
struct FCelestialEnables;

/**
 * The camera-following IsSky dome (ROADMAP 6A, docs/superpowers/specs/2026-10-16-night-sky-6a-design.md "Sky dome"): owns the
 * dome mesh component, its M_SkyDome dynamic instance, the transient BSC5 star texture, and writes MPC_Sky. Game thread.
 *
 * Frames: Cesium's ComputeEastSouthUpToUnrealTransformation is East-South-Up, so an ENU vector (E, N, U) goes in as (E, -N, U)
 * (EnuToWorldDir). Held as a UPROPERTY of its owner so its UObjects stay referenced.
 */
USTRUCT()
struct CAMSIMTEST_API FSkyDomeDriver
{
	GENERATED_BODY()

	static constexpr const TCHAR* MaterialPath = TEXT("/Game/Sky/M_SkyDome.M_SkyDome");
	static constexpr const TCHAR* MpcPath      = TEXT("/Game/Sky/MPC_Sky.MPC_Sky");
	static constexpr const TCHAR* MeshPath     = TEXT("/Engine/EngineMeshes/Sphere.Sphere");
	static constexpr double DomeRadiusCm = 5.0e8;     // 5,000 km: IsSky meshes are drawn only in the sky pass, at far depth
	static constexpr double MoonRadiusKm = 1737.4;
	static constexpr float  ExtinctionKV = 0.172f;    // V-band extinction (Krisciunas & Schaefer), MPC Floor.z

	/** Spawns the dome component on Owner, loads the material, MPC and stars. Safe to call once. */
	void Init(AActor* Owner, const FCamSimConfig::FCelestialConfig& Cfg);

	/** Writes MPC_Sky and moves the dome to the camera. EsuToUnreal = Cesium's East-South-Up → UE at the camera;
	 *  CenturiesJ2000 = Julian centuries since J2000.0 of the sky's time (precession of the J2000 star texture). */
	void Update(const FEphemerisResult& E, const FNightIlluminationResult& I, const FCelestialEnables& En,
		const FMatrix& EsuToUnreal, const FVector& CameraWorld, double LatDeg, double CenturiesJ2000);

	bool HasStars() const { return bHasStars; }
	bool IsInitialized() const { return Dome != nullptr; }
	/** Star texels with light (setup log). */
	int32 GetStarTexelCount() const { return StarTexelCount; }
	UStaticMeshComponent* GetDome() const { return Dome; }

	/** The moon disc's geometry in UE world (M_SkyDome's frame): moon and sun directions, the IAU body axes (J2000 → world
	 *  through SkyRotationRows), L(phase). DaysTt = days since J2000.0 TT. */
	static FMoonDiscFrame MoonFrame(const FEphemerisResult& E, const FMatrix& EsuToUnreal, const FVector Rows[3], double DaysTt);
	/** Days since J2000.0 TT of the instant CenturiesJ2000 (UT) describes: + ΔT. */
	static double DaysTtFromCenturiesUt(double CenturiesJ2000);
	/** The albedo map the disc and its normalisation use (empty: uniform disc). */
	const FMoonAlbedoMap& GetMoonAlbedo() const { return MoonAlbedo; }
	/** Normaliser recomputes so far and their total cost (camsim.Sky.Log). */
	const FMoonNormaliserCache& GetMoonNormaliser() const { return MoonNormaliser; }

	/** ENU (E, N, U) → unit UE world direction through Cesium's East-South-Up matrix. */
	static FVector EnuToWorldDir(const FMatrix& EsuToUnreal, const FVector3d& Enu);
	/**
	 * Rows of the UE world → J2000 equatorial rotation (x toward RA 0, y toward RA 90 deg, z the north celestial pole; the
	 * star texture's frame) at latitude LatDeg, local sidereal time LastDeg and CenturiesJ2000: world → ENU (the ESU frame,
	 * South negated) → equatorial of date with the zenith at (RA LAST, Dec latitude) → J2000 (inverse precession).
	 * MPC_Sky SkyRot0..2.
	 */
	static void SkyRotationRows(const FMatrix& EsuToUnreal, double LatDeg, double LastDeg, double CenturiesJ2000, FVector OutRows[3]);
	/** IAU 1976 precession (Meeus, Astronomical Algorithms ch. 21: zeta, z, theta in T = Julian centuries since J2000.0):
	 *  OutP (rows) maps a J2000 mean equatorial unit vector to the mean equator and equinox of date. */
	static void PrecessionJ2000ToDate(double CenturiesJ2000, FVector OutP[3]);
	/** Julian centuries since J2000.0 (JD 2451545.0) of a UTC instant (UT for TT: 70 s is 1e-4 arcsec of precession). */
	static double CenturiesSinceJ2000(const FDateTime& Utc);

private:
	UPROPERTY(Transient) TObjectPtr<UStaticMeshComponent>         Dome;
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic>     Material;
	UPROPERTY(Transient) TObjectPtr<UMaterialParameterCollection> Mpc;
	UPROPERTY(Transient) TObjectPtr<UTexture2D>                   StarTexture;
	UPROPERTY(Transient) TObjectPtr<UTexture2D>                   MoonTexture;

	FMoonAlbedoMap MoonAlbedo;
	/** The disc normaliser: recomputed only when the phase, sub-observer or sub-solar point moved > 0.05 deg. */
	FMoonNormaliserCache MoonNormaliser;
	double LastMoonMean = 1.0;

	bool  bHasStars = false;
	bool  bWarnedMpc = false;
	int32 StarTexelCount = 0;

	void SetVector(UWorld* World, const TCHAR* Name, const FLinearColor& Value);
};
