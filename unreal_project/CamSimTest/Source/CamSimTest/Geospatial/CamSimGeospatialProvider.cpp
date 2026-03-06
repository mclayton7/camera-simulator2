// Copyright CamSim Contributors. All Rights Reserved.

#include "Geospatial/CamSimGeospatialProvider.h"
#include "CamSimTest.h"

#include "CesiumGeoreference.h"
#include "Engine/World.h"

namespace
{
FString NormalizeProvider(const FString& Value)
{
	return Value.TrimStartAndEnd().ToLower();
}

ACesiumGeoreference* ResolveCesiumGeoreference(UWorld* World)
{
	return World ? ACesiumGeoreference::GetDefaultGeoreference(World) : nullptr;
}
}

FCamSimGeospatialProvider::FCamSimGeospatialProvider(const FCamSimConfig& InConfig)
{
	ProviderName = NormalizeProvider(InConfig.TerrainProvider);
	if (ProviderName.IsEmpty())
	{
		ProviderName = TEXT("cesium");
	}

	if (ProviderName != TEXT("cesium"))
	{
		UE_LOG(LogCamSim, Warning,
			TEXT("GeospatialProvider: unsupported terrain provider '%s' -> falling back to 'cesium'"),
			*ProviderName);
		ProviderName = TEXT("cesium");
	}

	Capabilities.bSupportsGeoreferenceTransforms = true;
	Capabilities.bSupportsTerrainLineTraceQueries = true;
}

bool FCamSimGeospatialProvider::IsAvailable(UWorld* World) const
{
	if (ProviderName == TEXT("cesium"))
	{
		return ResolveCesiumGeoreference(World) != nullptr;
	}
	return false;
}

bool FCamSimGeospatialProvider::GeoToWorld(
	UWorld* World, double Lat, double Lon, double AltM, FVector& OutWorld) const
{
	if (ProviderName == TEXT("cesium"))
	{
		if (ACesiumGeoreference* GeoRef = ResolveCesiumGeoreference(World))
		{
			OutWorld = GeoRef->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(Lon, Lat, AltM));
			return true;
		}
	}
	return false;
}

bool FCamSimGeospatialProvider::WorldToGeo(
	UWorld* World, const FVector& WorldPos, double& OutLat, double& OutLon, double& OutAltM) const
{
	if (ProviderName == TEXT("cesium"))
	{
		if (ACesiumGeoreference* GeoRef = ResolveCesiumGeoreference(World))
		{
			const FVector LLH = GeoRef->TransformUnrealPositionToLongitudeLatitudeHeight(WorldPos);
			OutLon = LLH.X;
			OutLat = LLH.Y;
			OutAltM = LLH.Z;
			return true;
		}
	}
	return false;
}

