// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Config/CamSimConfig.h"
#include "Entity/EntityTypeTable.h"
#include "CamSimSubsystem.generated.h"

class FCigiReceiver;
class IFrameSink;
class FCamSimEntityManager;
class FCigiSender;
class FCigiQueryHandler;

/**
 * UCamSimSubsystem
 *
 * Lifetime owner for the CIGI receiver and video encoder.  Created and torn
 * down automatically with the game instance so the camera actor can obtain
 * stable pointers via UGameInstance::GetSubsystem<UCamSimSubsystem>().
 */
UCLASS()
class CAMSIMTEST_API UCamSimSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Called once per game tick from FCamSimEntityManager::Tick().
	 * Drives FCigiQueryHandler + FCigiSender (SOF + response flush).
	 */
	void Tick(float DeltaTime);

	// Accessors used by ACamSimCamera and FCamSimEntityManager
	FCigiReceiver*        GetCigiReceiver()  const { return CigiReceiver; }
	IFrameSink*           GetVideoEncoder()  const { return VideoEncoder; }
	FCamSimEntityManager* GetEntityManager() const { return EntityManager; }
	FCigiSender*          GetCigiSender()    const { return CigiSender; }
	FCigiQueryHandler*    GetQueryHandler()  const { return QueryHandler; }

	const FCamSimConfig&    GetConfig()        const { return Config; }
	const FEntityTypeTable& GetEntityTypeTable() const { return EntityTypeTable; }

private:
	FCamSimConfig    Config;
	FEntityTypeTable EntityTypeTable;

	// Raw pointers — created/destroyed in Initialize/Deinitialize.
	// TUniquePtr<ForwardDeclaredType> cannot appear in a UCLASS header because
	// UHT generates an inline constructor that triggers C++2c's incomplete-type
	// delete check.
	FCigiReceiver*        CigiReceiver   = nullptr;
	IFrameSink*           VideoEncoder   = nullptr;
	FCamSimEntityManager* EntityManager  = nullptr;
	FCigiSender*          CigiSender     = nullptr;
	FCigiQueryHandler*    QueryHandler   = nullptr;

	// IG frame counter — incremented each tick; sent in every SOF packet
	uint32                FrameCntr      = 0;

	// Encoder watchdog — detect and recover from silent stream death
	// (avio_open succeeds but UDP sends silently fail after network change)
	uint64                WatchdogLastSuccessFrame = 0;   // snapshot of encoder's counter
	uint32                WatchdogLastCheckTick    = 0;   // tick when snapshot was taken
	uint32                WatchdogReconnectCount   = 0;

	// Runtime health snapshot counters (periodic diagnostics log)
	uint32                HealthLastTick           = 0;
	uint64                HealthLastSuccessFrame   = 0;
	uint64                HealthLastRxPacketCount  = 0;
};
