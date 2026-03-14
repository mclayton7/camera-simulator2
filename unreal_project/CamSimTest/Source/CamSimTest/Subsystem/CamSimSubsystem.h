// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/PimplPtr.h"
#include "Config/CamSimConfig.h"
#include "Entity/EntityTypeTable.h"
#include "CamSimSubsystem.generated.h"

class FCigiReceiver;
class IFrameSink;
class FCamSimEntityManager;
class FCigiSender;
class FCigiQueryHandler;
class FCamSimGeospatialProvider;
class FGroundTruthCollector;
class FCamSimParticleManager;
class ACamSimCamera;

/**
 * UCamSimSubsystem
 *
 * Lifetime owner for the CIGI receiver and video encoder.  Created and torn
 * down automatically with the game instance so the camera actor can obtain
 * stable pointers via UGameInstance::GetSubsystem<UCamSimSubsystem>().
 *
 * Phase 13B: Internal state is held via a Pimpl (FSubsystemImpl) allocated
 * in Initialize() and destroyed in Deinitialize().  This eliminates the
 * leak-on-exception risk from raw new/delete scattered across two methods.
 */
UCLASS()
class CAMSIMTEST_API UCamSimSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	UCamSimSubsystem();
	virtual ~UCamSimSubsystem() override;

	// USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Called once per game tick from FCamSimEntityManager::Tick().
	 * Drives FCigiQueryHandler + FCigiSender (SOF + response flush).
	 */
	void Tick(float DeltaTime);

	// Accessors used by ACamSimCamera and FCamSimEntityManager
	FCigiReceiver*        GetCigiReceiver()  const;
	IFrameSink*           GetVideoEncoder()  const;
	FCamSimEntityManager* GetEntityManager() const;
	FCigiSender*          GetCigiSender()    const;
	FCigiQueryHandler*    GetQueryHandler()  const;
	FCamSimGeospatialProvider* GetGeospatialProvider() const;
	FGroundTruthCollector* GetGroundTruthCollector() const;
	FCamSimParticleManager* GetParticleManager() const;

	const FCamSimConfig&    GetConfig()        const { return Config; }
	const FEntityTypeTable& GetEntityTypeTable() const { return EntityTypeTable; }

	// Phase 27B — camera registration for health JSON frame drop stats
	void             RegisterCamera(ACamSimCamera* Camera) { Camera_ = Camera; }
	ACamSimCamera*   GetCamera() const { return Camera_.Get(); }

private:
	FCamSimConfig    Config;
	FEntityTypeTable EntityTypeTable;

	// Phase 27B — weak reference to the camera actor (game thread only)
	TWeakObjectPtr<ACamSimCamera> Camera_;

	// Phase 13B: Pimpl — all owned subsystem components live in FSubsystemImpl,
	// defined in CamSimSubsystem.cpp.  TUniquePtr<> with a forward-declared type
	// requires the destructor to be defined in the .cpp, which the Pimpl pattern
	// handles naturally.  This replaces 6 raw new/delete pointer pairs.
	struct FSubsystemImpl;
	TPimplPtr<FSubsystemImpl> Impl;
};
