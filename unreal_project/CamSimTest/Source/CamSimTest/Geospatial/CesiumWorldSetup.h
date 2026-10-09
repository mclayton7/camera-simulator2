// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UCesiumIonServer;
class UGameInstance;
class UWorld;
struct FCamSimConfig;

namespace CamSim::Geospatial
{
	/**
	 * Origin-shift mobility, tileset tuning and the Cesium backend (one terrain tileset,
	 * every other tileset destroyed), in that order. Call before the world's actors begin
	 * play: ACesium3DTileset::BeginPlay calls LoadTileset, so Main.umap's ion actors would
	 * otherwise request api.cesium.com before CamSim's config reaches them (REALISM R0).
	 * Returns the ion server override, or nullptr (see ApplyCesiumBackendConfig).
	 */
	UCesiumIonServer* SetUpCesiumWorld(UWorld* World, const FCamSimConfig& Cfg);

	/**
	 * Whether a world reported by FWorldDelegates::OnPostWorldInitialization is one to set up:
	 * a game world whose game instance is Ours. That broadcast comes from
	 * InitWorld, before InitializeActorsForPlay re-runs construction scripts on uncooked levels
	 * (ACesium3DTileset::OnConstruction loads the tileset), so it is the first safe point.
	 */
	bool ShouldSetUpCesiumWorld(const UWorld* World, const UGameInstance* Ours);

	/** Runs SetUpCesiumWorld once per world: the game mode's StartPlay and the camera's BeginPlay both ask. */
	class FCesiumWorldSetupLatch
	{
	public:
		/** True the first time it sees World; false for nullptr or a repeat. */
		bool TryBegin(UWorld* World)
		{
			if (World == nullptr || Prepared.Get() == World)
			{
				return false;
			}
			Prepared = World;
			return true;
		}

	private:
		TWeakObjectPtr<UWorld> Prepared;
	};
}
