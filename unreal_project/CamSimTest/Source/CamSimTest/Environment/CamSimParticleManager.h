// Copyright CamSim Contributors. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UCamSimSubsystem;
struct FCamSimConfig;
struct FCigiComponentControl;
struct FCigiEntityState;
class UNiagaraComponent;
class UNiagaraSystem;
class UDecalComponent;
class AActor;

/**
 * Per-entity particle effect state. All pointers are game-thread only.
 * bContrailActive tracks whether contrail is currently emitting (altitude threshold).
 */
struct FEntityParticleState
{
    UNiagaraComponent* RotorWashComp  = nullptr;
    UNiagaraComponent* SmokeComp      = nullptr;
    UNiagaraComponent* FireComp       = nullptr;
    UNiagaraComponent* ContrailComp   = nullptr;
    bool               bSmokeActive   = false;
    bool               bFireActive    = false;
    bool               bContrailActive = false;
	UNiagaraComponent* WakeComp    = nullptr;
	bool               bWakeActive = false;
};

/**
 * Manages entity-attached Niagara particle effects and decal craters.
 * Owned by UCamSimSubsystem::FSubsystemImpl. Game-thread only.
 *
 * Entity IDs use uint16 throughout to match the rest of the entity system.
 *
 * Phase 18: 18F (rotor wash), 18G (smoke/fire), 18H (contrails), 18I (craters).
 * Note: contrail speed gate is not enforced — FCigiEntityState has no velocity field.
 * Contrails activate on altitude threshold only.
 */
class FCamSimParticleManager
{
public:
    explicit FCamSimParticleManager(UCamSimSubsystem* InSubsystem);
    ~FCamSimParticleManager() = default;

    void Initialize(const FCamSimConfig& Config);

    void OnEntitySpawned(uint16 EntityID, AActor* Actor, const FCigiEntityState& State);
    void OnEntityUpdated(uint16 EntityID, AActor* Actor, const FCigiEntityState& State);
    void OnEntityRemoved(uint16 EntityID);
    void OnComponentControl(uint16 EntityID, AActor* Actor, const FCigiComponentControl& Pkt);
    void Tick(float DeltaTime);

private:
    void SpawnRotorWash(uint16 EntityID, AActor* Actor);
    void SpawnWake(uint16 EntityID, AActor* Actor);
    void UpdateContrail(uint16 EntityID, AActor* Actor, float AltitudeM);
    void ActivateSmoke(uint16 EntityID, AActor* Actor);
    void ActivateFire(uint16 EntityID, AActor* Actor);
    void SpawnCraterDecal(AActor* Actor);
    void DetachAll(FEntityParticleState& State);

    static constexpr uint8 KindPlatform  = 1;
    static constexpr uint8 DomainAir     = 1;
    static constexpr uint8 CatFixedWing  = 2;
    static constexpr uint8 CatRotaryWing = 3;

    UCamSimSubsystem*   Subsystem      = nullptr;
    UNiagaraSystem*     RotorWashAsset = nullptr;
    UNiagaraSystem*     SmokeAsset     = nullptr;
    UNiagaraSystem*     FireAsset      = nullptr;
    UNiagaraSystem*     ContrailAsset  = nullptr;
    UNiagaraSystem*     WakeAsset                         = nullptr;
    UMaterialInterface* CraterMaterial = nullptr;

    float               WakeFadeTime                      = 8.0f;
    bool                Config_Phase19_VesselWakesEnabled = false;

    float ContrailAltM      = 8000.0f;
    int32 SmokeComponentID  = 1;
    int32 FireComponentID   = 2;
    int32 CraterComponentID = 10;
    int32 MaxCraters        = 32;
    float CraterRadiusM     = 5.0f;

    TMap<uint16, FEntityParticleState> EntityParticles;
    TArray<UDecalComponent*>           ActiveCraters;  // ring buffer
};
