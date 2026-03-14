// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Config/CamSimConfig.h"

class UWorld;
class UCesiumIonServer;

struct FCamSimGeospatialCapabilities
{
	bool bSupportsGeoreferenceTransforms = false;
	bool bSupportsTerrainLineTraceQueries = false;
};

/**
 * Provider-neutral geospatial transform facade.
 *
 * Phase F foundation:
 * - hides direct georeference implementation dependencies from query logic
 * - centralizes provider selection and capabilities
 */
class FCamSimGeospatialProvider
{
public:
	explicit FCamSimGeospatialProvider(const FCamSimConfig& InConfig);

	const FString& GetProviderName() const { return ProviderName; }
	const FCamSimGeospatialCapabilities& GetCapabilities() const { return Capabilities; }

	bool IsAvailable(UWorld* World) const;
	bool GeoToWorld(UWorld* World, double Lat, double Lon, double AltM, FVector& OutWorld) const;
	bool WorldToGeo(UWorld* World, const FVector& WorldPos, double& OutLat, double& OutLon, double& OutAltM) const;

private:
	FString ProviderName = TEXT("cesium");
	FCamSimGeospatialCapabilities Capabilities;
};

/**
 * Apply Cesium backend configuration (ion server, terrain source, imagery overlay)
 * to all ACesium3DTileset actors in the world.
 *
 * Must be called on the game thread.
 * Returns the created UCesiumIonServer* (nullptr if ion server step was skipped —
 * i.e. all three ion settings are at their defaults).
 * Caller is responsible for passing the result to UCamSimSubsystem::StoreCesiumIonServer()
 * to prevent GC.
 */
UCesiumIonServer* ApplyCesiumBackendConfig(
	UWorld* World, const FCamSimConfig::FCesiumBackendConfig& Config);

