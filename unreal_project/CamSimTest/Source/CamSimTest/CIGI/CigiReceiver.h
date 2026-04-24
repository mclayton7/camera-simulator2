// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "CIGI/BoundedSpscQueue.h"
#include "Config/CamSimConfig.h"
#include "CIGI/CigiPacketTypes.h"

// Forward declarations for CCL types (avoid pulling CCL headers into every TU)
class CigiBaseEventProcessor;
class CigiIncomingMsg;
class CigiIGSession;

/**
 * FCigiReceiver
 *
 * Background FRunnable thread that:
 *   - Binds a UDP socket on CigiPort
 *   - Feeds raw bytes into the CIGI Class Library (CCL) parser
 *   - Routes parsed packets into SPSC queues consumed by the game thread:
 *       CameraEntityQueue  – entity states for the camera entity (id == CameraEntityId)
 *       EntityStateQueue   – entity states for all other entities (FCamSimEntityManager)
 *       ViewDefQueue       – view definition packets (ACamSimCamera)
 *       SensorCtrlQueue    – sensor control packets  (ACamSimCamera)
 *       ViewCtrlQueue      – view control packets    (ACamSimCamera)
 *       CelestialQueue     – celestial control (UCamSimEnvironment)
 *       AtmosphereQueue    – atmosphere control (UCamSimEnvironment)
 *       WeatherQueue       – weather control    (UCamSimEnvironment)
 *       RateCtrlQueue      – rate control       (FCamSimEntityManager)
 *       ArtPartQueue       – art part control   (FCamSimEntityManager)
 *       CameraArtPartQueue – art part control targeting camera entity (ACamSimCamera)
 *       CompCtrlQueue      – component control  (FCamSimEntityManager)
 *
 * Thread safety: Only the game thread calls Dequeue*; only the receiver thread
 * calls the enqueue side.
 *
 * Phase 10 additions — terrain feedback queues:
 *   HatHotReqQueue  – HAT/HOT requests  (opcode 24) → FCigiQueryHandler
 *   LosSegReqQueue  – LOS segment reqs  (opcode 25) → FCigiQueryHandler
 *   LosVectReqQueue – LOS vector reqs   (opcode 26) → FCigiQueryHandler
 */
class FCigiReceiver : public FRunnable
{
public:
	explicit FCigiReceiver(const FCamSimConfig& InConfig);
	virtual ~FCigiReceiver();

	/** Starts the background thread. Returns false if socket bind fails. */
	bool Start();

	/** Signals the thread to exit and waits for it to finish. */
	void Stop();

	/** Number of UDP datagrams successfully read from the CIGI socket. */
	uint64 GetReceivedPacketCount() const { return ReceivedPacketCount.Load(); }

	/** Last host frame counter received from IG Control (opcode 1). */
	uint32 GetLastHostFrame() const { return LastHostFrameCntr.Load(); }

	// -----------------------------------------------------------------------
	// Game-thread SPSC accessors — one per queue.
	// Each method pops the oldest item from the named queue into Out and
	// returns true, or returns false immediately if the queue is empty.
	// -----------------------------------------------------------------------

#define CAMSIM_DEQUEUE(MethodName, QueueMember, OutType) \
	bool MethodName(OutType& Out) { return QueueMember.Dequeue(Out); }

	/** Camera entity states (EntityId == CameraEntityId). Consumed by ACamSimCamera. */
	CAMSIM_DEQUEUE(DequeueCameraEntityState, CameraEntityQueue,   FCigiEntityState)
	/** Non-camera entity states. Consumed by FCamSimEntityManager. */
	CAMSIM_DEQUEUE(DequeueEntityState,       EntityStateQueue,    FCigiEntityState)
	CAMSIM_DEQUEUE(DequeueViewDefinition,    ViewDefQueue,        FCigiViewDefinition)
	/** Sensor control packets (opcode 17). Consumed by ACamSimCamera. */
	CAMSIM_DEQUEUE(DequeueSensorControl,     SensorCtrlQueue,     FCigiSensorControl)
	/** View control packets (opcode 16). Consumed by ACamSimCamera. */
	CAMSIM_DEQUEUE(DequeueViewControl,       ViewCtrlQueue,       FCigiViewControl)
	/** Art part packets for the camera entity. Consumed by ACamSimCamera (gimbal). */
	CAMSIM_DEQUEUE(DequeueCameraArtPart,     CameraArtPartQueue,  FCigiArtPartControl)
	CAMSIM_DEQUEUE(DequeueCelestialState,    CelestialQueue,      FCigiCelestialState)
	CAMSIM_DEQUEUE(DequeueAtmosphereState,   AtmosphereQueue,     FCigiAtmosphereState)
	CAMSIM_DEQUEUE(DequeueWeatherState,      WeatherQueue,        FCigiWeatherState)
	/** Rate control packets. Consumed by FCamSimEntityManager. */
	CAMSIM_DEQUEUE(DequeueRateControl,       RateCtrlQueue,       FCigiRateControl)
	/** Articulated part control packets. Consumed by FCamSimEntityManager. */
	CAMSIM_DEQUEUE(DequeueArtPart,           ArtPartQueue,        FCigiArtPartControl)
	/** Component control packets. Consumed by FCamSimEntityManager. */
	CAMSIM_DEQUEUE(DequeueCompCtrl,          CompCtrlQueue,       FCigiComponentControl)
	/** HAT/HOT request packets (opcode 24). Consumed by FCigiQueryHandler. */
	CAMSIM_DEQUEUE(DequeueHatHotRequest,     HatHotReqQueue,      FCigiHatHotRequest)
	/** LOS segment request packets (opcode 25). Consumed by FCigiQueryHandler. */
	CAMSIM_DEQUEUE(DequeueLosSegRequest,     LosSegReqQueue,      FCigiLosSegRequest)
	/** LOS vector request packets (opcode 26). Consumed by FCigiQueryHandler. */
	CAMSIM_DEQUEUE(DequeueLosVectRequest,    LosVectReqQueue,     FCigiLosVectRequest)

#undef CAMSIM_DEQUEUE

	/** Dequeue a wave control state for ocean surface update. Returns false when queue is empty. */
	bool DequeueWaveState(FCigiWaveState& OutState) { return WaveStateQueue.Dequeue(OutState); }

	/** Phase 26D: Conformal clamped entity control (opcode 3). Consumed by FCamSimEntityManager. */
	bool DequeueConfClampEntity(FCigiConfClampEntityState& Out) { return ConfClampQueue.Dequeue(Out); }

	/** Phase 26D: Maritime surface conditions (opcode 13). Consumed by FOceanManager. */
	bool DequeueMaritimeSurface(FCigiMaritimeSurfaceState& Out) { return MaritimeSurfaceQueue.Dequeue(Out); }

	// FRunnable interface
	virtual bool   Init() override;
	virtual uint32 Run() override;
	virtual void   Exit() override;

private:
	// Processor classes (defined in .cpp) need access to the SPSC queues
	friend class FEntityCtrlProcessor;
	friend class FViewDefProcessor;
	friend class FSensorCtrlProcessor;
	friend class FViewCtrlProcessor;
	friend class FRateCtrlProcessor;
	friend class FArtPartProcessor;
	friend class FCompCtrlProcessor;
	friend class FHatHotReqProcessor;
	friend class FLosSegReqProcessor;
	friend class FLosVectReqProcessor;
	friend class FIGCtrlProcessor;
	friend class FWaveCtrlProcessor;
	friend class FConfClampProcessor;
	friend class FCollisionDetSegProcessor;
	friend class FMaritimeSurfaceProcessor;

	// CigiRawParse needs queue access for direct env packet parsing
	// (bypasses CCL's hold mechanism for celestial/atmos/weather packets)
	friend class FCigiRawEnvParser;

	const FCamSimConfig& Config;

	TUniquePtr<FRunnableThread> Thread;
	FSocket*         Socket    = nullptr;
	TAtomic<bool>    bShouldRun;
	// Auto-reset event used to wake the receiver thread immediately on Stop()
	// so shutdown doesn't depend on the non-blocking recv poll interval.
	// Event is owned by the platform pool and returned via ReturnSynchEventToPool,
	// so the raw pointer is intentional here.
	FEvent*          ShutdownEvent = nullptr;
	TAtomic<uint64>  ReceivedPacketCount { 0 };
	TAtomic<uint32>  LastHostFrameCntr { 0 };

	// Bounded SPSC queues: receiver thread produces, game thread consumes.
	// Camera entity is routed separately so ACamSimCamera and FCamSimEntityManager
	// each have their own exclusive consumer, preserving SPSC invariant.
	// Capacities are sized to absorb a ~1 s game-thread stall at typical CIGI
	// fan-in without unbounded heap growth. Drop-newest on overflow.
	static constexpr int32 CigiEntityQueueCapacity  = 2048;  // many entities per-frame
	static constexpr int32 CigiControlQueueCapacity = 512;   // view/sensor/rate
	static constexpr int32 CigiRequestQueueCapacity = 1024;  // HAT/HOT/LOS bursts

	TBoundedSpscQueue<FCigiEntityState>      CameraEntityQueue  { CigiControlQueueCapacity };
	TBoundedSpscQueue<FCigiEntityState>      EntityStateQueue   { CigiEntityQueueCapacity };
	TBoundedSpscQueue<FCigiViewDefinition>   ViewDefQueue       { CigiControlQueueCapacity };
	TBoundedSpscQueue<FCigiSensorControl>    SensorCtrlQueue    { CigiControlQueueCapacity };  // opcode 17
	TBoundedSpscQueue<FCigiViewControl>      ViewCtrlQueue      { CigiControlQueueCapacity };  // opcode 16
	TBoundedSpscQueue<FCigiCelestialState>   CelestialQueue     { CigiControlQueueCapacity };
	TBoundedSpscQueue<FCigiAtmosphereState>  AtmosphereQueue    { CigiControlQueueCapacity };
	TBoundedSpscQueue<FCigiWeatherState>     WeatherQueue       { CigiControlQueueCapacity };
	TBoundedSpscQueue<FCigiRateControl>      RateCtrlQueue      { CigiEntityQueueCapacity };
	TBoundedSpscQueue<FCigiArtPartControl>   ArtPartQueue       { CigiEntityQueueCapacity };
	TBoundedSpscQueue<FCigiArtPartControl>   CameraArtPartQueue { CigiControlQueueCapacity };
	TBoundedSpscQueue<FCigiComponentControl> CompCtrlQueue      { CigiEntityQueueCapacity };
	TBoundedSpscQueue<FCigiHatHotRequest>    HatHotReqQueue     { CigiRequestQueueCapacity };  // opcode 24
	TBoundedSpscQueue<FCigiLosSegRequest>    LosSegReqQueue     { CigiRequestQueueCapacity };  // opcode 25
	TBoundedSpscQueue<FCigiLosVectRequest>   LosVectReqQueue    { CigiRequestQueueCapacity };  // opcode 26
	TBoundedSpscQueue<FCigiWaveState>        WaveStateQueue     { CigiControlQueueCapacity };  // opcode 14
	TBoundedSpscQueue<FCigiConfClampEntityState> ConfClampQueue { CigiEntityQueueCapacity };   // opcode 3
	TBoundedSpscQueue<FCigiMaritimeSurfaceState> MaritimeSurfaceQueue { CigiControlQueueCapacity }; // opcode 13

public:
	/** Aggregate total drops across all CIGI queues — exposed for /metrics. */
	uint64 GetTotalDropCount() const;
private:

	// CCL session objects — session owns InMsg; we hold a non-owning pointer to it
	TUniquePtr<CigiIGSession> CigiSession;
	CigiIncomingMsg*          IncomingMsg = nullptr;

	// CCL event processor instances (registered with IncomingMsg; lifetime
	// must exceed IncomingMsg's, so stored here as owning pointers)
	TUniquePtr<CigiBaseEventProcessor> EntityCtrlProc;
	TUniquePtr<CigiBaseEventProcessor> ViewDefProc;
	TUniquePtr<CigiBaseEventProcessor> SensorCtrlProc;
	TUniquePtr<CigiBaseEventProcessor> ViewCtrlProc;
	TUniquePtr<CigiBaseEventProcessor> RateCtrlProc;
	TUniquePtr<CigiBaseEventProcessor> ArtPartProc;
	TUniquePtr<CigiBaseEventProcessor> CompCtrlProc;
	TUniquePtr<CigiBaseEventProcessor> HatHotReqProc;
	TUniquePtr<CigiBaseEventProcessor> LosSegReqProc;
	TUniquePtr<CigiBaseEventProcessor> LosVectReqProc;
	TUniquePtr<CigiBaseEventProcessor> IGCtrlProc;
	TUniquePtr<CigiBaseEventProcessor> WaveCtrlProc;
	TUniquePtr<CigiBaseEventProcessor> ConfClampProc;
	TUniquePtr<CigiBaseEventProcessor> CollisionDetSegProc;
	TUniquePtr<CigiBaseEventProcessor> MaritimeSurfaceProc;

	bool CreateSocket();

	// Phase 12E: CIGI input recording
	// Raw UDP datagrams written as [uint64 timestamp_us][uint32 length][bytes...]
	IFileHandle* RecordFileHandle = nullptr;

	// Phase 12E: CIGI playback from recorded file
	bool          bPlaybackMode = false;
};
