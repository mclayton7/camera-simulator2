// Copyright CamSim Contributors. All Rights Reserved.

#include "CIGI/CigiSender.h"
#include "CamSimTest.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Common/UdpSocketBuilder.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

THIRD_PARTY_INCLUDES_START
#include "cigicl/CigiIGSession.h"
#include "cigicl/CigiOutgoingMsg.h"
#include "cigicl/CigiSOFV3.h"
#include "cigicl/CigiHatHotRespV3.h"
#include "cigicl/CigiLosRespV3.h"
THIRD_PARTY_INCLUDES_END

// -------------------------------------------------------------------------
// Constructor / Destructor
// -------------------------------------------------------------------------

FCigiSender::FCigiSender()
{
}

FCigiSender::~FCigiSender()
{
	Close();
}

// -------------------------------------------------------------------------
// Open / Close
// -------------------------------------------------------------------------

bool FCigiSender::Open(const FCamSimConfig& Config)
{
	// Build destination address
	ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SS)
	{
		UE_LOG(LogCamSim, Error, TEXT("FCigiSender: no socket subsystem"));
		return false;
	}

	DestAddr = SS->CreateInternetAddr();
	bool bIsValid = false;
	DestAddr->SetIp(*Config.CigiResponseAddr, bIsValid);
	if (!bIsValid)
	{
		UE_LOG(LogCamSim, Error, TEXT("FCigiSender: invalid response address '%s'"),
			*Config.CigiResponseAddr);
		return false;
	}
	DestAddr->SetPort(Config.CigiResponsePort);

	// Create non-blocking UDP send socket (no bind required for send-only)
	Socket = FUdpSocketBuilder(TEXT("CigiSenderSocket"))
		.AsNonBlocking()
		.AsReusable()
		.WithSendBufferSize(64 * 1024)
		.Build();

	if (!Socket)
	{
		UE_LOG(LogCamSim, Error, TEXT("FCigiSender: failed to create UDP socket"));
		return false;
	}

	// CCL outgoing-only session: 0 in-buffers, 2 out-buffers of 4096 bytes each
	CigiSession = new CigiIGSession(0, 0, 2, 4096);
	CigiSession->SetCigiVersion(3, 3);

	OutgoingMsg = &CigiSession->GetOutgoingMsgMgr();

	// Pre-allocate SOF packet (reused every frame)
	SofPacket = new CigiSOFV3();

	bOpen = true;
	UE_LOG(LogCamSim, Log, TEXT("FCigiSender: sending to %s:%d"),
		*Config.CigiResponseAddr, Config.CigiResponsePort);
	return true;
}

void FCigiSender::Close()
{
	if (!bOpen) return;
	bOpen = false;

	// Free any unreleased pending packets
	for (CigiHatHotRespV3* P : PendingHatHot) { delete P; }
	PendingHatHot.Empty();
	for (CigiLosRespV3* P : PendingLos) { delete P; }
	PendingLos.Empty();

	delete SofPacket;   SofPacket   = nullptr;
	delete CigiSession; CigiSession = nullptr;  // also destroys OutgoingMsg
	OutgoingMsg = nullptr;

	if (Socket)
	{
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}

	UE_LOG(LogCamSim, Log, TEXT("FCigiSender: closed"));
}

// -------------------------------------------------------------------------
// Per-frame flush
// -------------------------------------------------------------------------

void FCigiSender::FlushFrame(uint32 FrameCntr, uint8 LastHostFrame)
{
	if (!bOpen || !OutgoingMsg || !SofPacket) return;

	// Update SOF frame counter
	SofPacket->SetFrameCntr(static_cast<Cigi_uint32>(FrameCntr));

	// Begin message assembly
	OutgoingMsg->BeginMsg();

	// SOF must be first
	*OutgoingMsg << *SofPacket;

	// Pack staged HAT/HOT responses
	for (CigiHatHotRespV3* P : PendingHatHot)
	{
		*OutgoingMsg << *static_cast<CigiBasePacket*>(P);
		delete P;
	}
	PendingHatHot.Empty();

	// Pack staged LOS responses
	for (CigiLosRespV3* P : PendingLos)
	{
		*OutgoingMsg << *static_cast<CigiBasePacket*>(P);
		delete P;
	}
	PendingLos.Empty();

	// Finalise and send
	Cigi_uint8* MsgBuf = nullptr;
	int MsgLen = 0;
	if (OutgoingMsg->PackageMsg(&MsgBuf, MsgLen) == CIGI_SUCCESS && MsgBuf && MsgLen > 0)
	{
		int32 BytesSent = 0;
		Socket->SendTo(reinterpret_cast<const uint8*>(MsgBuf), MsgLen, BytesSent, *DestAddr);
		OutgoingMsg->FreeMsg();
	}
}

// -------------------------------------------------------------------------
// Response staging
// -------------------------------------------------------------------------

void FCigiSender::EnqueueHatHotResponse(uint16 HatHotId, bool bValid,
                                        uint8 ReqType, double Hat, double Hot)
{
	if (!bOpen) return;

	CigiHatHotRespV3* Resp = new CigiHatHotRespV3();
	Resp->SetHatHotID(static_cast<Cigi_uint16>(HatHotId));
	Resp->SetValid(bValid);
	// ReqType: 0=HAT, 1=HOT, 2=Extended.
	// CIGI v3.3 HAT/HOT response supports only basic HAT/HOT tagging; map
	// extended (2) to HOT semantics in the response.
	const uint8 ResponseReqType = (ReqType == 1 || ReqType == 2) ? 1 : 0;
	Resp->SetReqType(static_cast<CigiBaseHatHotResp::ReqTypeGrp>(ResponseReqType));
	if (bValid)
	{
		Resp->SetHat(Hat);
		Resp->SetHot(Hot);
	}
	PendingHatHot.Add(Resp);
}

void FCigiSender::EnqueueLosResponse(uint16 LosId, bool bValid, bool bVisible,
                                     double Range, double HitLat, double HitLon,
                                     double HitAlt, uint16 EntityId, bool bEntityIdValid)
{
	if (!bOpen) return;

	CigiLosRespV3* Resp = new CigiLosRespV3();
	Resp->SetLosID(static_cast<Cigi_uint16>(LosId));
	Resp->SetValid(bValid);
	Resp->SetVisible(bVisible);
	Resp->SetRespCount(1);
	if (bValid)
	{
		Resp->SetRange(Range);
		Resp->SetLatitude(HitLat);
		Resp->SetLongitude(HitLon);
		Resp->SetAltitude(HitAlt);
	}
	Resp->SetEntityIDValid(bEntityIdValid);
	if (bEntityIdValid)
	{
		Resp->SetEntityID(static_cast<Cigi_uint16>(EntityId));
	}
	PendingLos.Add(Resp);
}
