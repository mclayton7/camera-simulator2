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
#include "cigicl/CigiSOFV3_2.h"
#include "cigicl/CigiBaseSOF.h"        // CigiBaseSOF::IGModeGrp (Phase 12D)
#include "cigicl/CigiHatHotRespV3.h"
#include "cigicl/CigiLosRespV3.h"
#include "cigicl/CigiSensorXRespV3.h"
#include "cigicl/CigiBaseSensorResp.h"
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
	CigiSession = MakeUnique<CigiIGSession>(0, 0, 2, 4096);
	CigiSession->SetCigiVersion(3, 3);

	OutgoingMsg = &CigiSession->GetOutgoingMsgMgr();

	// Pre-allocate the SOF and the single-slot SensorX response. Both are
	// reused every frame; their fields are overwritten before each send.
	SofPacket = MakeUnique<CigiSOFV3_2>();
	SensorXResp = MakeUnique<CigiSensorXRespV3>();

	bOpen = true;
	UE_LOG(LogCamSim, Log, TEXT("FCigiSender: sending to %s:%d"),
		*Config.CigiResponseAddr, Config.CigiResponsePort);
	return true;
}

void FCigiSender::Close()
{
	if (!bOpen) return;
	bOpen = false;

	// Pools own their CCL packets via TUniquePtr; clearing releases them.
	HatHotPool.Empty();
	HatHotInUse = 0;
	LosPool.Empty();
	LosInUse = 0;

	SensorXResp.Reset();
	bSensorXRespPending = false;

	SofPacket.Reset();
	CigiSession.Reset();  // destruction also invalidates OutgoingMsg
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

void FCigiSender::FlushFrame(uint32 FrameCntr, uint8 LastHostFrame, uint8 IGMode)
{
	if (!bOpen || !OutgoingMsg || !SofPacket)
	{
		UE_LOG(LogCamSim, Verbose,
			TEXT("FCigiSender::FlushFrame skipped — sender not open (frame=%u)"), FrameCntr);
		return;
	}

	// Update SOF frame counter, host frame echo, and IG operating mode (Phase 12D)
	SofPacket->SetFrameCntr(static_cast<Cigi_uint32>(FrameCntr));
	SofPacket->SetLastRcvdHostFrame(static_cast<Cigi_uint32>(LastHostFrame));
	SofPacket->SetIGMode(static_cast<CigiBaseSOF::IGModeGrp>(FMath::Clamp((int)IGMode, 0, 2)));

	// Begin message assembly
	OutgoingMsg->BeginMsg();

	// SOF must be first
	*OutgoingMsg << *SofPacket;

	// Serialise staged HAT/HOT responses from the pool prefix, then reset the
	// counter — the pool slots themselves are retained for next frame.
	for (int32 Idx = 0; Idx < HatHotInUse; ++Idx)
	{
		*OutgoingMsg << *static_cast<CigiBasePacket*>(HatHotPool[Idx].Get());
	}
	HatHotInUse = 0;

	// Serialise staged LOS responses the same way.
	for (int32 Idx = 0; Idx < LosInUse; ++Idx)
	{
		*OutgoingMsg << *static_cast<CigiBasePacket*>(LosPool[Idx].Get());
	}
	LosInUse = 0;

	// Pack the single-slot sensor extended response if one was staged.
	if (bSensorXRespPending && SensorXResp.IsValid())
	{
		*OutgoingMsg << *static_cast<CigiBasePacket*>(SensorXResp.Get());
		bSensorXRespPending = false;
	}

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
	if (!bOpen)
	{
		UE_LOG(LogCamSim, Verbose,
			TEXT("FCigiSender::EnqueueHatHotResponse dropped (hatHotId=%u) — sender not open"),
			HatHotId);
		return;
	}

	// Grow the pool on demand. Steady state: the pool plateaus after the first
	// few frames at the peak per-frame response count and no further allocs run.
	if (HatHotInUse >= HatHotPool.Num())
	{
		HatHotPool.Emplace(MakeUnique<CigiHatHotRespV3>());
	}
	CigiHatHotRespV3* Resp = HatHotPool[HatHotInUse++].Get();

	Resp->SetHatHotID(static_cast<Cigi_uint16>(HatHotId));
	Resp->SetValid(bValid);
	// ReqType: 0=HAT, 1=HOT, 2=Extended.
	// CIGI v3.3 HAT/HOT response supports only basic HAT/HOT tagging; map
	// extended (2) to HOT semantics in the response.
	const uint8 ResponseReqType = (ReqType == 1 || ReqType == 2) ? 1 : 0;
	Resp->SetReqType(static_cast<CigiBaseHatHotResp::ReqTypeGrp>(ResponseReqType));
	// Always write Hat/Hot, even when bValid is false — otherwise a recycled
	// slot could surface stale metres from a prior valid response.
	Resp->SetHat(bValid ? Hat : 0.0);
	Resp->SetHot(bValid ? Hot : 0.0);
}

void FCigiSender::EnqueueLosResponse(uint16 LosId, bool bValid, bool bVisible,
                                     double Range, double HitLat, double HitLon,
                                     double HitAlt, uint16 EntityId, bool bEntityIdValid)
{
	if (!bOpen)
	{
		UE_LOG(LogCamSim, Verbose,
			TEXT("FCigiSender::EnqueueLosResponse dropped (losId=%u) — sender not open"),
			LosId);
		return;
	}

	if (LosInUse >= LosPool.Num())
	{
		LosPool.Emplace(MakeUnique<CigiLosRespV3>());
	}
	CigiLosRespV3* Resp = LosPool[LosInUse++].Get();

	Resp->SetLosID(static_cast<Cigi_uint16>(LosId));
	Resp->SetValid(bValid);
	Resp->SetVisible(bVisible);
	Resp->SetRespCount(1);
	// Always write hit fields so a recycled slot never surfaces stale data.
	Resp->SetRange   (bValid ? Range  : 0.0);
	Resp->SetLatitude(bValid ? HitLat : 0.0);
	Resp->SetLongitude(bValid ? HitLon : 0.0);
	Resp->SetAltitude(bValid ? HitAlt : 0.0);
	Resp->SetEntityIDValid(bEntityIdValid);
	Resp->SetEntityID(bEntityIdValid ? static_cast<Cigi_uint16>(EntityId) : 0);
}

void FCigiSender::SetSensorResponse(uint16 ViewId, uint8 SensorId, uint8 SensorStat,
                                     float GateXoff, float GateYoff,
                                     uint16 GateSzX, uint16 GateSzY,
                                     double TrackLat, double TrackLon, double TrackAlt,
                                     uint32 FrameCntr)
{
	if (!bOpen || !SensorXResp.IsValid())
	{
		UE_LOG(LogCamSim, Verbose,
			TEXT("FCigiSender::SetSensorResponse dropped (viewId=%u sensorId=%u) — sender not open"),
			ViewId, SensorId);
		return;
	}

	// Reuse the single preallocated slot. Caller semantics unchanged: repeated
	// calls within a single frame overwrite the staged response.
	SensorXResp->SetViewID(static_cast<Cigi_uint16>(ViewId));
	SensorXResp->SetSensorID(static_cast<Cigi_uint8>(SensorId));
	SensorXResp->SetSensorStat(
		static_cast<CigiBaseSensorResp::SensorStatGrp>(FMath::Clamp((int)SensorStat, 0, 2)));
	SensorXResp->SetGateXoff(GateXoff);
	SensorXResp->SetGateYoff(GateYoff);
	SensorXResp->SetGateSzX(static_cast<Cigi_uint16>(GateSzX));
	SensorXResp->SetGateSzY(static_cast<Cigi_uint16>(GateSzY));
	SensorXResp->SetEntityTgt(false);
	SensorXResp->SetTrackPntLat(TrackLat);
	SensorXResp->SetTrackPntLon(TrackLon);
	SensorXResp->SetTrackPntAlt(TrackAlt);
	SensorXResp->SetFrameCntr(static_cast<Cigi_uint32>(FrameCntr));
	bSensorXRespPending = true;
}
