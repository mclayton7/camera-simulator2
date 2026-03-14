// Copyright CamSim Contributors. All Rights Reserved.
#include "Environment/CamSimParticleManager.h"
#include "CamSimTest.h"
#include "Config/CamSimConfig.h"
#include "Subsystem/CamSimSubsystem.h"
#include "CIGI/CigiPacketTypes.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraFunctionLibrary.h"
#include "Components/DecalComponent.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"

FCamSimParticleManager::FCamSimParticleManager(UCamSimSubsystem* InSubsystem)
    : Subsystem(InSubsystem)
{}

void FCamSimParticleManager::Initialize(const FCamSimConfig& Config)
{
    ContrailAltM      = Config.Phase18.ContrailAltM;
    SmokeComponentID  = Config.Phase18.SmokeComponentID;
    FireComponentID   = Config.Phase18.FireComponentID;
    CraterComponentID = Config.Phase18.CraterImpactComponentID;
    MaxCraters        = Config.Phase18.MaxCraters;
    CraterRadiusM     = Config.Phase18.CraterDefaultRadiusM;

    auto LoadNiagara = [](const FString& Path) -> UNiagaraSystem*
    {
        UNiagaraSystem* A = LoadObject<UNiagaraSystem>(nullptr, *Path);
        if (!A)
        {
            UE_LOG(LogCamSim, Warning,
                TEXT("CamSimParticleManager: Niagara asset not found: '%s'. "
                     "Author it in the UE editor — see README Gotchas."), *Path);
        }
        return A;
    };
    RotorWashAsset = LoadNiagara(Config.Phase18.NiagaraRotorWash);
    SmokeAsset     = LoadNiagara(Config.Phase18.NiagaraSmoke);
    FireAsset      = LoadNiagara(Config.Phase18.NiagaraFire);
    ContrailAsset  = LoadNiagara(Config.Phase18.NiagaraContrail);
    WakeAsset           = LoadNiagara(Config.Phase19.NiagaraVesselWake);
    WakeFadeTime        = Config.Phase19.WakeFadeTime;
    bVesselWakesEnabled = Config.Phase19.bVesselWakesEnabled;

    CraterMaterial = LoadObject<UMaterialInterface>(nullptr, *Config.Phase18.CraterDecalMaterial);
    if (!CraterMaterial)
    {
        UE_LOG(LogCamSim, Warning,
            TEXT("CamSimParticleManager: Crater material not found: '%s'."),
            *Config.Phase18.CraterDecalMaterial);
    }
}

void FCamSimParticleManager::OnEntitySpawned(uint16 EntityID, AActor* Actor,
                                              const FCigiEntityState& State)
{
    if (!Actor) return;
    EntityParticles.FindOrAdd(EntityID);

    // 18F: Rotary-wing entities get rotor wash immediately on spawn
    if (State.EntityKind == KindPlatform && State.EntityDomain == DomainAir
        && State.EntityCategory == CatRotaryWing)
    {
        SpawnRotorWash(EntityID, Actor);
    }

	// 19B: Sea-domain entities get wake FX
	if (bVesselWakesEnabled &&
	    State.EntityDomain == 3 && WakeAsset)
	{
		SpawnWake(EntityID, Actor);
	}
}

void FCamSimParticleManager::OnEntityUpdated(uint16 EntityID, AActor* Actor,
                                              const FCigiEntityState& State)
{
    if (!Actor) return;

    // 18H: Fixed-wing contrails — altitude threshold only (speed not in FCigiEntityState)
    if (State.EntityKind == KindPlatform && State.EntityDomain == DomainAir
        && State.EntityCategory == CatFixedWing)
    {
        UpdateContrail(EntityID, Actor, State.Altitude);
    }
}

void FCamSimParticleManager::OnEntityRemoved(uint16 EntityID)
{
    if (FEntityParticleState* PS = EntityParticles.Find(EntityID))
    {
        DetachAll(*PS);
        EntityParticles.Remove(EntityID);
    }
}

void FCamSimParticleManager::OnComponentControl(uint16 EntityID, AActor* Actor,
                                                 const FCigiComponentControl& Pkt)
{
    if (!Actor) return;
    if (Pkt.CompId == static_cast<uint16>(SmokeComponentID))  { ActivateSmoke(EntityID, Actor); return; }
    if (Pkt.CompId == static_cast<uint16>(FireComponentID))   { ActivateFire(EntityID, Actor);  return; }
    if (Pkt.CompId == static_cast<uint16>(CraterComponentID)) { SpawnCraterDecal(Actor);         return; }
}

void FCamSimParticleManager::Tick(float DeltaTime)
{
    // Reserved for future per-frame parameter updates
}

// ─── Private ──────────────────────────────────────────────────────────────────

void FCamSimParticleManager::SpawnRotorWash(uint16 EntityID, AActor* Actor)
{
    if (!RotorWashAsset) return;
    FEntityParticleState& PS = EntityParticles.FindOrAdd(EntityID);
    if (PS.RotorWashComp) return;
    PS.RotorWashComp = UNiagaraFunctionLibrary::SpawnSystemAttached(
        RotorWashAsset, Actor->GetRootComponent(),
        NAME_None, FVector::ZeroVector, FRotator::ZeroRotator,
        EAttachLocation::KeepRelativeOffset, /*bAutoDestroy=*/false);
}

void FCamSimParticleManager::SpawnWake(uint16 EntityID, AActor* Actor)
{
	FEntityParticleState& PS = EntityParticles.FindOrAdd(EntityID);
	if (PS.WakeComp || !WakeAsset) return;

	PS.WakeComp = UNiagaraFunctionLibrary::SpawnSystemAttached(
		WakeAsset, Actor->GetRootComponent(),
		NAME_None, FVector::ZeroVector, FRotator::ZeroRotator,
		EAttachLocation::KeepRelativeOffset, false);

	if (PS.WakeComp)
	{
		PS.WakeComp->SetFloatParameter(FName("FadeTime"), WakeFadeTime);
		PS.bWakeActive = true;
	}
}

void FCamSimParticleManager::UpdateContrail(uint16 EntityID, AActor* Actor, float AltitudeM)
{
    FEntityParticleState& PS = EntityParticles.FindOrAdd(EntityID);
    const bool bShouldBeActive = (AltitudeM >= ContrailAltM);

    if (bShouldBeActive && !PS.bContrailActive)
    {
        if (!PS.ContrailComp && ContrailAsset)
        {
            PS.ContrailComp = UNiagaraFunctionLibrary::SpawnSystemAttached(
                ContrailAsset, Actor->GetRootComponent(),
                NAME_None, FVector::ZeroVector, FRotator::ZeroRotator,
                EAttachLocation::KeepRelativeOffset, /*bAutoDestroy=*/false);
        }
        if (PS.ContrailComp) { PS.ContrailComp->Activate(true); }
        PS.bContrailActive = true;
    }
    else if (!bShouldBeActive && PS.bContrailActive)
    {
        if (PS.ContrailComp) { PS.ContrailComp->Deactivate(); }
        PS.bContrailActive = false;
    }
}

void FCamSimParticleManager::ActivateSmoke(uint16 EntityID, AActor* Actor)
{
    if (!SmokeAsset) return;
    FEntityParticleState& PS = EntityParticles.FindOrAdd(EntityID);
    if (PS.bSmokeActive) return;
    if (!PS.SmokeComp)
    {
        PS.SmokeComp = UNiagaraFunctionLibrary::SpawnSystemAttached(
            SmokeAsset, Actor->GetRootComponent(),
            NAME_None, FVector::ZeroVector, FRotator::ZeroRotator,
            EAttachLocation::KeepRelativeOffset, /*bAutoDestroy=*/false);
    }
    if (PS.SmokeComp) { PS.SmokeComp->Activate(true); }
    PS.bSmokeActive = true;
}

void FCamSimParticleManager::ActivateFire(uint16 EntityID, AActor* Actor)
{
    if (!FireAsset) return;
    FEntityParticleState& PS = EntityParticles.FindOrAdd(EntityID);
    if (PS.bFireActive) return;
    if (!PS.FireComp)
    {
        PS.FireComp = UNiagaraFunctionLibrary::SpawnSystemAttached(
            FireAsset, Actor->GetRootComponent(),
            NAME_None, FVector::ZeroVector, FRotator::ZeroRotator,
            EAttachLocation::KeepRelativeOffset, /*bAutoDestroy=*/false);
    }
    if (PS.FireComp) { PS.FireComp->Activate(true); }
    PS.bFireActive = true;
}

void FCamSimParticleManager::SpawnCraterDecal(AActor* Actor)
{
    if (!CraterMaterial || !Actor || !Actor->GetWorld()) return;

    const FVector Start = Actor->GetActorLocation();
    const FVector End   = Start - FVector(0.0f, 0.0f, 100000.0f);   // 1 km down
    FHitResult Hit;
    if (!Actor->GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility)) return;

    const float RadiusCm = CraterRadiusM * 100.0f;
    UDecalComponent* Decal = UGameplayStatics::SpawnDecalAtLocation(
        Actor->GetWorld(), CraterMaterial,
        FVector(RadiusCm * 2.0f, RadiusCm * 2.0f, RadiusCm),
        Hit.Location, Hit.Normal.Rotation(), /*LifeSpan=*/0.0f);
    if (!Decal) return;
    Decal->SetFadeScreenSize(0.0001f);

    if (ActiveCraters.Num() >= MaxCraters)
    {
        if (UDecalComponent* Oldest = ActiveCraters[0]) { Oldest->DestroyComponent(); }
        ActiveCraters.RemoveAt(0);
    }
    ActiveCraters.Add(Decal);
}

void FCamSimParticleManager::DetachAll(FEntityParticleState& PS)
{
    auto Destroy = [](UNiagaraComponent*& Comp)
    {
        if (Comp) { Comp->DeactivateImmediate(); Comp->DestroyComponent(); Comp = nullptr; }
    };
    Destroy(PS.RotorWashComp);
    Destroy(PS.SmokeComp);
    Destroy(PS.FireComp);
    Destroy(PS.ContrailComp);
    Destroy(PS.WakeComp);
    PS.bSmokeActive    = false;
    PS.bFireActive     = false;
    PS.bContrailActive = false;
    PS.bWakeActive     = false;
}
