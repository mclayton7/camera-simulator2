// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Config/CamSimConfig.h"

// Forward-declare CCL types to avoid pulling heavy headers into every TU
class CigiIGSession;
class CigiOutgoingMsg;
class CigiSOFV3_2;
class CigiHatHotRespV3;
class CigiLosRespV3;
class CigiSensorXRespV3;

/**
 * FCigiSender
 *
 * Sends CIGI IG-to-host packets over UDP.  Each call to FlushFrame() emits
 * one datagram containing:
 *   1. Start-of-Frame (SOF, opcode 101) — mandatory CIGI heartbeat
 *   2. Any HAT/HOT response packets staged via EnqueueHatHotResponse()
 *   3. Any LOS response packets staged via EnqueueLosResponse()
 *   4. A Sensor Extended Response (opcode 107) staged via SetSensorResponse()
 *
 * Uses a dedicated CigiIGSession (outgoing only) so it does not share
 * state with the receiver's session.
 *
 * Thread safety: All methods must be called from the game thread.
 */
class FCigiSender
{
public:
	FCigiSender();
	~FCigiSender();

	/** Create the UDP socket and initialise CCL outgoing session.
	 *  Returns false on failure (socket create / bind error). */
	bool Open(const FCamSimConfig& Config);

	/** Close socket and release CCL resources. */
	void Close();

	/**
	 * Pack SOF + all pending responses into one datagram and send it.
	 * @param FrameCntr      IG frame counter (incremented by caller each tick)
	 * @param LastHostFrame  Least-significant nibble of last host frame received
	 * @param IGMode         IG operating mode: 0=Standby, 1=Operate, 2=Debug
	 */
	void FlushFrame(uint32 FrameCntr, uint8 LastHostFrame, uint8 IGMode = 1);

	/** Stage a HAT/HOT response for the current frame. */
	void EnqueueHatHotResponse(uint16 HatHotId, bool bValid, uint8 ReqType,
	                           double Hat, double Hot);

	/** Stage a LOS response for the current frame. */
	void EnqueueLosResponse(uint16 LosId, bool bValid, bool bVisible,
	                        double Range, double HitLat, double HitLon, double HitAlt,
	                        uint16 EntityId, bool bEntityIdValid);

	/** Stage a Sensor Extended Response for the current frame.
	 *  Provides gimbal look-point and sensor status to the host. */
	void SetSensorResponse(uint16 ViewId, uint8 SensorId, uint8 SensorStat,
	                        float GateXoff, float GateYoff,
	                        uint16 GateSzX, uint16 GateSzY,
	                        double TrackLat, double TrackLon, double TrackAlt,
	                        uint32 FrameCntr);

private:
	FSocket*                      Socket      = nullptr;
	TSharedPtr<FInternetAddr>     DestAddr;

	// Separate outgoing-only CCL session (no incoming buffers needed)
	CigiIGSession*  CigiSession  = nullptr;
	CigiOutgoingMsg* OutgoingMsg = nullptr;  // non-owning ptr into CigiSession
	CigiSOFV3_2*    SofPacket   = nullptr;

	// Per-frame staging arrays (cleared after FlushFrame)
	TArray<CigiHatHotRespV3*> PendingHatHot;
	TArray<CigiLosRespV3*>    PendingLos;

	CigiSensorXRespV3* PendingSensorXResp = nullptr;

	bool bOpen = false;
};
