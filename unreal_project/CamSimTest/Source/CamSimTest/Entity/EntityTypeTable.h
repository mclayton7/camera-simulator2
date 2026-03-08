// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

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
 * Lookup table mapping CIGI EntityType uint16 → FEntityTypeEntry.
 * Populated at startup from the "entity_types" block in camsim_config.yaml.
 *
 * YAML format:
 *   entity_types:
 *     "1001":
 *       mesh: "/Game/..."
 *       skeletal: true
 *       mesh_damaged: "/Game/..."
 *       mesh_destroyed: "/Game/..."
 *       scale: 1.0
 *       rotation:
 *         pitch: 0.0
 *         yaw: 0.0
 *         roll: 0.0
 */
class UStaticMesh;
class USkeletalMesh;

class FEntityTypeTable
{
public:
	/** Re-parse the config YAML and populate the type map from "entity_types". */
	void LoadFromConfig();

	/** Returns the entry for the given type ID, or nullptr if not found. */
	const FEntityTypeEntry* FindEntry(uint16 TypeId) const;

	/** Retrieve or store a cached static mesh for the given type ID. */
	UStaticMesh* GetCachedStaticMesh(uint16 TypeId) const;
	void SetCachedStaticMesh(uint16 TypeId, UStaticMesh* Mesh);

	/** Retrieve or store a cached skeletal mesh for the given type ID. */
	USkeletalMesh* GetCachedSkeletalMesh(uint16 TypeId) const;
	void SetCachedSkeletalMesh(uint16 TypeId, USkeletalMesh* Mesh);

private:
	TMap<uint16, FEntityTypeEntry> TypeMap;

	// Mesh cache: avoids re-parsing glTF for the same type (Phase 4)
	mutable TMap<uint16, TWeakObjectPtr<UStaticMesh>> StaticMeshCache;
	mutable TMap<uint16, TWeakObjectPtr<USkeletalMesh>> SkeletalMeshCache;
};
