// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Config/CamSimConfig.h"

// Forward-declare CCL types to avoid pulling heavy headers into every TU
class CigiIGSession;
class CigiOutgoingMsg;
class CigiSOFV3;
class CigiHatHotRespV3;
class CigiLosRespV3;

/**
 * FCigiSender
 *
 * Sends CIGI IG-to-host packets over UDP.  Each call to FlushFrame() emits
 * one datagram containing:
 *   1. Start-of-Frame (SOF, opcode 101) — mandatory CIGI heartbeat
 *   2. Any HAT/HOT response packets staged via EnqueueHatHotResponse()
 *   3. Any LOS response packets staged via EnqueueLosResponse()
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
	 */
	void FlushFrame(uint32 FrameCntr, uint8 LastHostFrame);

	/** Stage a HAT/HOT response for the current frame. */
	void EnqueueHatHotResponse(uint16 HatHotId, bool bValid, uint8 ReqType,
	                           double Hat, double Hot);

	/** Stage a LOS response for the current frame. */
	void EnqueueLosResponse(uint16 LosId, bool bValid, bool bVisible,
	                        double Range, double HitLat, double HitLon, double HitAlt,
	                        uint16 EntityId, bool bEntityIdValid);

private:
	FSocket*                      Socket      = nullptr;
	TSharedPtr<FInternetAddr>     DestAddr;

	// Separate outgoing-only CCL session (no incoming buffers needed)
	CigiIGSession*  CigiSession  = nullptr;
	CigiOutgoingMsg* OutgoingMsg = nullptr;  // non-owning ptr into CigiSession
	CigiSOFV3*      SofPacket   = nullptr;

	// Per-frame staging arrays (cleared after FlushFrame)
	TArray<CigiHatHotRespV3*> PendingHatHot;
	TArray<CigiLosRespV3*>    PendingLos;

	bool bOpen = false;
};
