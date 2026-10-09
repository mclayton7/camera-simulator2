// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sim/Commands.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/PimplPtr.h"
#include "Config/CamSimConfig.h"
#include "Entity/EntityTypeTable.h"
#include "Geospatial/CigiFrames.h"
#include "Geospatial/CesiumWorldSetup.h"
#include "CamSimSubsystem.generated.h"

class FCigiReceiver;
class IFrameSink;
class FCamSimEntityManager;
class FCigiSender;
class FCigiQueryHandler;
class FCamSimGeospatialProvider;
class FGroundTruthCollector;
class FCamSimSnapshotService;
class FDisReceiver;
class FDisEntityAdapter;
class FOceanSurface;
class ACamSimCamera;
class ACesium3DTileset;
class UCesiumIonServer;
struct FPipelineLatencyTracker;

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
	 * Drives FCigiQueryHandler and stages this frame's FCigiSender datagram
	 * (sent by FlushCigiFrame).
	 */
	void Tick(float DeltaTime);

	/**
	 * Send this frame's CIGI IG-to-host datagram: Start of Frame, the staged
	 * HAT/HOT and LOS responses, and the Sensor Extended Response (opcode 107)
	 * with the camera's frame centre. Tick() stages the frame; ACamSimCamera
	 * calls this once it has computed this frame's frame centre, so the
	 * response's centre and frame number match. Without a camera Tick() sends
	 * it; a frame the camera missed goes out at the start of the next Tick().
	 * Game thread; once per frame (later calls do nothing).
	 */
	void FlushCigiFrame();

	// Accessors used by ACamSimCamera and FCamSimEntityManager
	FCigiReceiver*        GetCigiReceiver()  const;
	IFrameSink*           GetVideoEncoder()  const;
	FCamSimEntityManager* GetEntityManager() const;
	FCigiSender*          GetCigiSender()    const;
	FCigiQueryHandler*    GetQueryHandler()  const;
	FCamSimGeospatialProvider* GetGeospatialProvider() const;
	FGroundTruthCollector* GetGroundTruthCollector() const;

	/** GET /snapshot service (ROADMAP 3A); null unless operational.snapshot_endpoint_enabled. */
	FCamSimSnapshotService* GetSnapshotService() const;
	/** GET /snapshot/sensor service (ROADMAP 3B); null unless operational.snapshot_endpoint_enabled. */
	FCamSimSnapshotService* GetSensorSnapshotService() const;
	FDisReceiver*          GetDisReceiver()     const;
	FDisEntityAdapter*     GetDisAdapter()      const;
	FPipelineLatencyTracker* GetLatencyTracker() const;

	/** The sea (ROADMAP 2.6); nullptr when ocean.enabled is off or the EGM96 grid is missing. */
	FOceanSurface*       GetOceanSurface();
	const FOceanSurface* GetOceanSurface() const;
	/** Apply the configured waves (Beaufort, direction, choppiness). */
	void ApplyOceanConfig(const FCamSimConfig::FOceanConfig& Cfg);

	/** Store the transient UCesiumIonServer created by ApplyCesiumBackendConfig.
	 *  Passing nullptr clears the stored reference (no-op if already null).
	 *  Must be called on the game thread. */
	void StoreCesiumIonServer(UCesiumIonServer* Server);

	/** REALISM R0: Cesium setup for World, once (ACamSimGameMode::StartPlay, else ACamSimCamera::BeginPlay). */
	void PrepareCesiumWorld(UWorld* World);

	/**
	 * The config, loaded once in Initialize. Cross-thread readers snapshot the
	 * fields they need at construction, as the receivers and encoder do.
	 */
	const FCamSimConfig&    GetConfig()        const { return Config; }
	const FEntityTypeTable& GetEntityTypeTable() const { return EntityTypeTable; }

	/**
	 * ROADMAP 3B: whether the GPU sensor graph (the only sensor path) runs this
	 * session, decided once in Initialize by CanRunSensorGraph. When false no
	 * frames are produced and /ready stays false; the reason is logged as an error.
	 */
	bool IsSensorGraphAvailable() const { return bSensorGraphAvailable; }

	/**
	 * Ground truth (ROADMAP 2.7): whether entities are stencil-tagged and the instance-ID pass runs this
	 * session — the sensor graph runs, ml_training.enabled && bounding_boxes, and InstanceIdCS is available.
	 * Decided once in Initialize (live config changes don't flip it); false: projected boxes only.
	 */
	bool IsGroundTruthMaskAvailable() const { return bGroundTruthMaskAvailable; }

	/**
	 * ROADMAP 4A: whether ThermalCS runs this session — the sensor graph runs, thermal.enabled, and ThermalCS is in the
	 * shader map. Decided once in Initialize; false: IR uses the visible-light proxy.
	 */
	bool IsThermalAvailable() const { return bThermalAvailable; }

	/** Whether entities get custom-depth stencil values: ground-truth masks or thermal need them. */
	bool IsEntityStencilTaggingEnabled() const { return IsGroundTruthMaskAvailable() || IsThermalAvailable(); }

	/**
	 * ML depth map (Phase 17A): whether InstanceIdCS's depth output is read back this session — the sensor
	 * graph runs, ml_training.enabled && depth_map, and InstanceIdCS is available. Decided once in Initialize.
	 * Independent of bounding_boxes (depth needs no stencil tags).
	 */
	bool IsGroundTruthDepthAvailable() const { return bGroundTruthDepthAvailable; }

	/**
	 * The config preconditions (primary view, NV12 dimensions), then
	 * IsSensorGraphSupported (a real RHI with SM5 compute and the shaders).
	 * Game thread, after RHI init. On false, OutWhy says what's missing.
	 */
	static bool CanRunSensorGraph(const FCamSimConfig& Cfg, FString& OutWhy);

	// Phase 27B — camera registration for /metrics frame drop stats
	void             RegisterCamera(ACamSimCamera* Camera);
	ACamSimCamera*   GetCamera() const;

	/**
	 * Current geodetic pose of an entity — the camera's platform for the
	 * configured camera entity (CIGI namespace), otherwise a managed entity.
	 * Used to resolve entity-relative coordinates (attachment, HAT/HOT, LOS).
	 * Returns false if the entity doesn't exist.
	 */
	bool GetEntityGeoPose(const FEntityKey& Key, CamSimFrames::FGeoPose& OutPose) const;

	/**
	 * Phase 3: cached tileset pointer list. Populated lazily on first access
	 * (and refreshable on demand) so per-tick loops in ACamSimCamera::Tick
	 * don't run TActorIterator every frame.
	 * TWeakObjectPtr handles destroyed-tileset cleanup naturally.
	 */
	const TArray<TWeakObjectPtr<ACesium3DTileset>>& GetCachedTilesets() const;

	/** Force a refresh of the cached tileset list. Must be called on the game thread. */
	void RefreshCachedTilesets();

private:
	FCamSimConfig    Config;
	bool             bSensorGraphAvailable = false;
	bool             bGroundTruthMaskAvailable = false;
	bool             bThermalAvailable = false;
	bool             bGroundTruthDepthAvailable = false;
	FEntityTypeTable EntityTypeTable;

	// Phase 27B — weak reference to the camera actor (game thread only)
	TWeakObjectPtr<ACamSimCamera> Camera_;

	// Phase 3: cached tileset pointer list (see public accessor).
	mutable TArray<TWeakObjectPtr<ACesium3DTileset>> CachedTilesets_;
	CamSim::Geospatial::FCesiumWorldSetupLatch CesiumWorldSetup_;
	FDelegateHandle PostWorldInitHandle_;
	mutable bool bCachedTilesetsInitialized_ = false;

	// Phase 13B: Pimpl — all owned subsystem components live in FSubsystemImpl,
	// defined in CamSimSubsystem.cpp.  TUniquePtr<> with a forward-declared type
	// requires the destructor to be defined in the .cpp, which the Pimpl pattern
	// handles naturally.  This replaces 6 raw new/delete pointer pairs.
	struct FSubsystemImpl;
	TPimplPtr<FSubsystemImpl> Impl;
};
