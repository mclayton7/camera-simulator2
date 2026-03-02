// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * FEntityTypeEntry
 *
 * Asset references and flags for a single entity model type.
 * AssetPath is the primary UObject path (Static or Skeletal mesh).
 * Damaged / Destroyed variants are optional; fall back to primary if empty.
 */
struct FEntityTypeEntry
{
	FString AssetPath;           // e.g. "/Game/Models/F16/F16.F16"
	FString DamagedAssetPath;    // optional — used for CompId=10, state=1
	FString DestroyedAssetPath;  // optional — used for CompId=10, state=2
	bool    bSkeletal = false;   // true → USkeletalMesh; false → UStaticMesh

	// Model-space correction applied to the mesh component's relative transform.
	// Use these to fix glTF assets that are scaled or oriented incorrectly.
	float   ModelScale    = 1.0f;   // uniform scale factor (default 1.0)
	FRotator ModelRotation = FRotator::ZeroRotator; // pitch/yaw/roll offset (degrees)
};

/**
 * FEntityTypeTable
 *
 * Singleton lookup table mapping CIGI EntityType uint16 → FEntityTypeEntry.
 * Populated at startup from the "entity_types" block in camsim_config.json.
 *
 * JSON format:
 *   "entity_types": {
 *     "1001": { "mesh": "/Game/...", "skeletal": true,
 *               "mesh_damaged": "/Game/...", "mesh_destroyed": "/Game/...",
 *               "scale": 1.0,
 *               "rotation": { "pitch": 0.0, "yaw": 0.0, "roll": 0.0 } },
 *     "2001": { "mesh": "/Game/...", "skeletal": false }
 *   }
 */
class FEntityTypeTable
{
public:
	/** Parse the "entity_types" JSON object and populate the type map. */
	void LoadFromConfig(const TSharedPtr<FJsonObject>& Root);

	/** Returns the entry for the given type ID, or nullptr if not found. */
	const FEntityTypeEntry* FindEntry(uint16 TypeId) const;

private:
	TMap<uint16, FEntityTypeEntry> TypeMap;
};
