// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Config/CamSimConfig.h"
#include "Ocean/OceanMesh.h"
#include "Ocean/OceanMeshBuilder.h"

class UCamSimSubsystem;
class UWorld;
class AActor;
class ACesiumGeoreference;
class UMaterialParameterCollection;
class FOceanSurface;
struct FOceanWaveCommand;
struct FMaritimeSurfaceCommand;
struct FCamSimTelemetry;

namespace CamSimOcean
{
	/**
	 * Write the wave set and the plane frame to MPC_Ocean (M_Ocean's inputs):
	 * Wave{i} = (k 1/m, a m, Q, phase rad), Dir{i} = (dN, dE, 0, 0), zeros past
	 * the active waves; ComponentToAnchor = ComponentWorld - anchor (UE cm);
	 * AxisN/E/U = the anchor's ENU axes as UE world unit vectors; Water =
	 * (absorption scale, scattering scale, ripple, 0) from the clarity.
	 */
	CAMSIMTEST_API void WriteMpc(UWorld* World, UMaterialParameterCollection* Mpc, const FOceanSurface& Ocean,
		const FVector& ComponentWorld, const FMatrix& EcefToUe);
}

/**
 * Draws the subsystem's FOceanSurface (ROADMAP 2.6) and applies CIGI ocean
 * commands to it. Lives inside ACamSimEnvironment.
 */
class FOceanManager
{
public:
	void Init(UWorld* World, AActor* Owner, UCamSimSubsystem* Subsystem);
	/** Camera = the sensor's telemetry this frame (null: nothing to draw around). */
	void Tick(const FCamSimTelemetry* Camera);
	void ApplyWave(const FOceanWaveCommand& Cmd);
	void ApplyMaritimeSurface(const FMaritimeSurfaceCommand& Cmd);

private:
	void CheckEcefUnitsOnce(const FMatrix& EcefToUe);

	UCamSimSubsystem* Subsystem = nullptr;
	bool bWarnedScopedWave = false;
	bool bWarnedWaveId = false;
	bool bWarnedScopedMaritime = false;

	FOceanMesh Mesh;
	TWeakObjectPtr<UMaterialParameterCollection> Mpc;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<ACesiumGeoreference> Georeference;
	CamSimOcean::FRebuildPolicy Last{ 0.0, 0.0, 0.0, false };
	FMatrix LastEcefToUe = FMatrix::Identity;   // georeference the mesh was built in (origin shifts rebuild)
	double LastNadirLat = 0.0, LastNadirLon = 0.0;
	bool bHasNadir = false;
	bool bUnitsChecked = false;
	FCamSimConfig::FOceanConfig Cfg;
};
