// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Containers/SpscQueue.h"
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

	// -----------------------------------------------------------------------
	// Game-thread accessors (SPSC — no locking needed)
	// -----------------------------------------------------------------------

	/** Camera entity states (EntityId == CameraEntityId). Consumed by ACamSimCamera. */
	bool DequeueCameraEntityState(FCigiEntityState& OutState)
	{
		return CameraEntityQueue.Dequeue(OutState);
	}

	/** Non-camera entity states. Consumed by FCamSimEntityManager. */
	bool DequeueEntityState(FCigiEntityState& OutState)
	{
		return EntityStateQueue.Dequeue(OutState);
	}

	bool DequeueViewDefinition(FCigiViewDefinition& OutView)
	{
		return ViewDefQueue.Dequeue(OutView);
	}

	/** Sensor control packets (opcode 17). Consumed by ACamSimCamera. */
	bool DequeueSensorControl(FCigiSensorControl& OutSensor)
	{
		return SensorCtrlQueue.Dequeue(OutSensor);
	}

	/** View control packets (opcode 16). Consumed by ACamSimCamera. */
	bool DequeueViewControl(FCigiViewControl& OutView)
	{
		return ViewCtrlQueue.Dequeue(OutView);
	}

	/** Art part packets targeting the camera entity. Consumed by ACamSimCamera for gimbal. */
	bool DequeueCameraArtPart(FCigiArtPartControl& OutArt)
	{
		return CameraArtPartQueue.Dequeue(OutArt);
	}

	bool DequeueCelestialState(FCigiCelestialState& OutState)
	{
		return CelestialQueue.Dequeue(OutState);
	}

	bool DequeueAtmosphereState(FCigiAtmosphereState& OutState)
	{
		return AtmosphereQueue.Dequeue(OutState);
	}

	bool DequeueWeatherState(FCigiWeatherState& OutState)
	{
		return WeatherQueue.Dequeue(OutState);
	}

	/** Rate control packets. Consumed by FCamSimEntityManager. */
	bool DequeueRateControl(FCigiRateControl& OutRate)
	{
		return RateCtrlQueue.Dequeue(OutRate);
	}

	/** Articulated part control packets. Consumed by FCamSimEntityManager. */
	bool DequeueArtPart(FCigiArtPartControl& OutArt)
	{
		return ArtPartQueue.Dequeue(OutArt);
	}

	/** Component control packets. Consumed by FCamSimEntityManager. */
	bool DequeueCompCtrl(FCigiComponentControl& OutComp)
	{
		return CompCtrlQueue.Dequeue(OutComp);
	}

	/** HAT/HOT request packets (opcode 24). Consumed by FCigiQueryHandler. */
	bool DequeueHatHotRequest(FCigiHatHotRequest& Out)
	{
		return HatHotReqQueue.Dequeue(Out);
	}

	/** LOS segment request packets (opcode 25). Consumed by FCigiQueryHandler. */
	bool DequeueLosSegRequest(FCigiLosSegRequest& Out)
	{
		return LosSegReqQueue.Dequeue(Out);
	}

	/** LOS vector request packets (opcode 26). Consumed by FCigiQueryHandler. */
	bool DequeueLosVectRequest(FCigiLosVectRequest& Out)
	{
		return LosVectReqQueue.Dequeue(Out);
	}

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

	// CigiRawParse needs queue access for direct env packet parsing
	// (bypasses CCL's hold mechanism for celestial/atmos/weather packets)
	friend class FCigiRawEnvParser;

	const FCamSimConfig& Config;

	FRunnableThread* Thread    = nullptr;
	FSocket*         Socket    = nullptr;
	TAtomic<bool>    bShouldRun;

	// SPSC queues: receiver thread produces, game thread consumes.
	// Camera entity is routed separately so ACamSimCamera and FCamSimEntityManager
	// each have their own exclusive consumer, preserving SPSC invariant.
	TSpscQueue<FCigiEntityState>      CameraEntityQueue;   // camera entity only
	TSpscQueue<FCigiEntityState>      EntityStateQueue;    // all other entities
	TSpscQueue<FCigiViewDefinition>   ViewDefQueue;
	TSpscQueue<FCigiSensorControl>    SensorCtrlQueue;     // opcode 17, ACamSimCamera
	TSpscQueue<FCigiViewControl>      ViewCtrlQueue;       // opcode 16, ACamSimCamera
	TSpscQueue<FCigiCelestialState>   CelestialQueue;
	TSpscQueue<FCigiAtmosphereState>  AtmosphereQueue;
	TSpscQueue<FCigiWeatherState>     WeatherQueue;
	TSpscQueue<FCigiRateControl>      RateCtrlQueue;
	TSpscQueue<FCigiArtPartControl>   ArtPartQueue;
	TSpscQueue<FCigiArtPartControl>   CameraArtPartQueue;  // art parts for camera entity (gimbal)
	TSpscQueue<FCigiComponentControl> CompCtrlQueue;
	TSpscQueue<FCigiHatHotRequest>    HatHotReqQueue;      // opcode 24 (FCigiQueryHandler)
	TSpscQueue<FCigiLosSegRequest>    LosSegReqQueue;      // opcode 25 (FCigiQueryHandler)
	TSpscQueue<FCigiLosVectRequest>   LosVectReqQueue;     // opcode 26 (FCigiQueryHandler)

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

	bool CreateSocket();
};
