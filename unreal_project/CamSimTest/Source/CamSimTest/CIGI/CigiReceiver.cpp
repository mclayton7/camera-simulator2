// Copyright CamSim Contributors. All Rights Reserved.

#include "CIGI/CigiReceiver.h"
#include "CamSimTest.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Common/UdpSocketBuilder.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

// CCL headers (static lib, drop-in) — wrapped to suppress third-party warnings
THIRD_PARTY_INCLUDES_START
#include "cigicl/CigiIGSession.h"
#include "cigicl/CigiIncomingMsg.h"
#include "cigicl/CigiBaseEventProcessor.h"
#include "cigicl/CigiEntityCtrlV3.h"
#include "cigicl/CigiViewDefV3.h"
#include "cigicl/CigiRateCtrlV3.h"      // opcode 8
#include "cigicl/CigiArtPartCtrlV3.h"   // opcode 6
#include "cigicl/CigiCompCtrlV3.h"      // opcode 4
#include "cigicl/CigiSensorCtrlV3.h"    // opcode 17
#include "cigicl/CigiViewCtrlV3.h"      // opcode 16
#include "cigicl/CigiHatHotReqV3.h"     // opcode 24
#include "cigicl/CigiLosSegReqV3.h"     // opcode 25
#include "cigicl/CigiLosVectReqV3.h"    // opcode 26
// CigiCelestialCtrl.h, CigiAtmosCtrl.h, CigiWeatherCtrlV3.h are NOT included
// here — we parse those packet types directly from raw bytes in
// FCigiRawEnvParser to bypass CCL's CigiHoldEnvCtrl merge mechanism.
// Weather opcode macro lives in CigiBaseWeatherCtrl.h; define it locally:
#ifndef CIGI_WEATHER_CTRL_PACKET_ID_V3
#define CIGI_WEATHER_CTRL_PACKET_ID_V3 12
#endif
THIRD_PARTY_INCLUDES_END

// -------------------------------------------------------------------------
// CCL event processor subclasses (defined here; friended by FCigiReceiver)
// -------------------------------------------------------------------------

class FEntityCtrlProcessor : public CigiBaseEventProcessor
{
	FCigiReceiver* Receiver;
public:
	explicit FEntityCtrlProcessor(FCigiReceiver* R) : Receiver(R) {}

	void OnPacketReceived(CigiBasePacket* Packet) override
	{
		auto* Pkt = static_cast<CigiEntityCtrlV3*>(Packet);
		if (!Receiver || !Pkt) return;

		FCigiEntityState State;
		State.EntityId    = static_cast<uint16>(Pkt->GetEntityID());
		State.EntityState = static_cast<uint8>(Pkt->GetEntityState());
		State.EntityType  = static_cast<uint16>(Pkt->GetEntityType());
		State.Latitude    = Pkt->GetLat();
		State.Longitude   = Pkt->GetLon();
		State.Altitude    = static_cast<float>(Pkt->GetAlt());
		State.Yaw         = static_cast<float>(Pkt->GetYaw());
		State.Pitch       = static_cast<float>(Pkt->GetPitch());
		State.Roll        = static_cast<float>(Pkt->GetRoll());

		// Route by entity ID: camera entity → CameraEntityQueue; others → EntityStateQueue
		if (State.EntityId == static_cast<uint16>(Receiver->Config.CameraEntityId))
		{
			Receiver->CameraEntityQueue.Enqueue(State);
		}
		else
		{
			Receiver->EntityStateQueue.Enqueue(State);
		}
	}
};

class FViewDefProcessor : public CigiBaseEventProcessor
{
	FCigiReceiver* Receiver;
public:
	explicit FViewDefProcessor(FCigiReceiver* R) : Receiver(R) {}

	void OnPacketReceived(CigiBasePacket* Packet) override
	{
		auto* Pkt = static_cast<CigiViewDefV3*>(Packet);
		if (!Receiver || !Pkt) return;

		FCigiViewDefinition View;
		View.ViewId    = static_cast<uint16>(Pkt->GetViewID());
		View.GroupId   = static_cast<uint8>(Pkt->GetGroupID());
		View.FovLeft   = static_cast<float>(Pkt->GetFOVLeft());
		View.FovRight  = static_cast<float>(Pkt->GetFOVRight());
		View.FovTop    = static_cast<float>(Pkt->GetFOVTop());
		View.FovBottom = static_cast<float>(Pkt->GetFOVBottom());
		View.NearPlane = 0.1f;
		View.FarPlane  = 1e6f;

		Receiver->ViewDefQueue.Enqueue(View);
	}
};

class FRateCtrlProcessor : public CigiBaseEventProcessor
{
	FCigiReceiver* Receiver;
public:
	explicit FRateCtrlProcessor(FCigiReceiver* R) : Receiver(R) {}

	void OnPacketReceived(CigiBasePacket* Packet) override
	{
		auto* Pkt = static_cast<CigiRateCtrlV3*>(Packet);
		if (!Receiver || !Pkt) return;

		FCigiRateControl Rate;
		Rate.EntityId        = static_cast<uint16>(Pkt->GetEntityID());
		Rate.ArtPartId       = static_cast<uint8>(Pkt->GetArtPartID());
		Rate.bApplyToArtPart = Pkt->GetApplyToArtPart();
		Rate.XRate           = static_cast<float>(Pkt->GetXRate());
		Rate.YRate           = static_cast<float>(Pkt->GetYRate());
		Rate.ZRate           = static_cast<float>(Pkt->GetZRate());
		Rate.RollRate        = static_cast<float>(Pkt->GetRollRate());
		Rate.PitchRate       = static_cast<float>(Pkt->GetPitchRate());
		Rate.YawRate         = static_cast<float>(Pkt->GetYawRate());

		Receiver->RateCtrlQueue.Enqueue(Rate);
	}
};

class FArtPartProcessor : public CigiBaseEventProcessor
{
	FCigiReceiver* Receiver;
public:
	explicit FArtPartProcessor(FCigiReceiver* R) : Receiver(R) {}

	void OnPacketReceived(CigiBasePacket* Packet) override
	{
		auto* Pkt = static_cast<CigiArtPartCtrlV3*>(Packet);
		if (!Receiver || !Pkt) return;

		FCigiArtPartControl Art;
		Art.EntityId   = static_cast<uint16>(Pkt->GetEntityID());
		Art.ArtPartId  = static_cast<uint8>(Pkt->GetArtPartID());
		Art.bArtPartEn = Pkt->GetArtPartEn();
		Art.bXOffEn    = Pkt->GetXOffEn();
		Art.bYOffEn    = Pkt->GetYOffEn();
		Art.bZOffEn    = Pkt->GetZOffEn();
		Art.bRollEn    = Pkt->GetRollEn();
		Art.bPitchEn   = Pkt->GetPitchEn();
		Art.bYawEn     = Pkt->GetYawEn();
		Art.XOff       = static_cast<float>(Pkt->GetXOff());
		Art.YOff       = static_cast<float>(Pkt->GetYOff());
		Art.ZOff       = static_cast<float>(Pkt->GetZOff());
		Art.Roll       = static_cast<float>(Pkt->GetRoll());
		Art.Pitch      = static_cast<float>(Pkt->GetPitch());
		Art.Yaw        = static_cast<float>(Pkt->GetYaw());

		// Route to camera gimbal queue or general entity queue
		if (Art.EntityId == static_cast<uint16>(Receiver->Config.CameraEntityId))
		{
			Receiver->CameraArtPartQueue.Enqueue(Art);
		}
		else
		{
			Receiver->ArtPartQueue.Enqueue(Art);
		}
	}
};

class FCompCtrlProcessor : public CigiBaseEventProcessor
{
	FCigiReceiver* Receiver;
public:
	explicit FCompCtrlProcessor(FCigiReceiver* R) : Receiver(R) {}

	void OnPacketReceived(CigiBasePacket* Packet) override
	{
		auto* Pkt = static_cast<CigiCompCtrlV3*>(Packet);
		if (!Receiver || !Pkt) return;

		FCigiComponentControl Comp;
		Comp.EntityId  = static_cast<uint16>(Pkt->GetInstanceID());  // CCL uses InstanceID
		Comp.CompId    = static_cast<uint16>(Pkt->GetCompID());
		Comp.CompClass = static_cast<uint8>(Pkt->GetCompClassV3());
		Comp.CompState = static_cast<uint8>(Pkt->GetCompState());

		Receiver->CompCtrlQueue.Enqueue(Comp);
	}
};

class FSensorCtrlProcessor : public CigiBaseEventProcessor
{
	FCigiReceiver* Receiver;
public:
	explicit FSensorCtrlProcessor(FCigiReceiver* R) : Receiver(R) {}

	void OnPacketReceived(CigiBasePacket* Packet) override
	{
		auto* Pkt = static_cast<CigiSensorCtrlV3*>(Packet);
		if (!Receiver || !Pkt) return;

		FCigiSensorControl Sensor;
		Sensor.ViewId    = static_cast<uint16>(Pkt->GetViewID());
		Sensor.SensorId  = static_cast<uint8>(Pkt->GetSensorID());
		Sensor.bSensorOn = Pkt->GetSensorOn();
		Sensor.Polarity  = static_cast<uint8>(Pkt->GetPolarity());
		Sensor.TrackMode = static_cast<uint8>(Pkt->GetTrackMode());
		Sensor.Gain      = static_cast<float>(Pkt->GetGain());

		Receiver->SensorCtrlQueue.Enqueue(Sensor);
	}
};

class FViewCtrlProcessor : public CigiBaseEventProcessor
{
	FCigiReceiver* Receiver;
public:
	explicit FViewCtrlProcessor(FCigiReceiver* R) : Receiver(R) {}

	void OnPacketReceived(CigiBasePacket* Packet) override
	{
		auto* Pkt = static_cast<CigiViewCtrlV3*>(Packet);
		if (!Receiver || !Pkt) return;

		FCigiViewControl View;
		View.ViewId   = static_cast<uint16>(Pkt->GetViewID());
		View.EntityId = static_cast<uint16>(Pkt->GetEntityID());
		View.GroupId  = static_cast<uint8>(Pkt->GetGroupID());
		View.bXOffEn  = Pkt->GetXOffEn();
		View.bYOffEn  = Pkt->GetYOffEn();
		View.bZOffEn  = Pkt->GetZOffEn();
		View.bRollEn  = Pkt->GetRollEn();
		View.bPitchEn = Pkt->GetPitchEn();
		View.bYawEn   = Pkt->GetYawEn();
		View.XOff     = static_cast<float>(Pkt->GetXOff());
		View.YOff     = static_cast<float>(Pkt->GetYOff());
		View.ZOff     = static_cast<float>(Pkt->GetZOff());
		View.Roll     = static_cast<float>(Pkt->GetRoll());
		View.Pitch    = static_cast<float>(Pkt->GetPitch());
		View.Yaw      = static_cast<float>(Pkt->GetYaw());

		Receiver->ViewCtrlQueue.Enqueue(View);
	}
};

class FHatHotReqProcessor : public CigiBaseEventProcessor
{
	FCigiReceiver* Receiver;
public:
	explicit FHatHotReqProcessor(FCigiReceiver* R) : Receiver(R) {}

	void OnPacketReceived(CigiBasePacket* Packet) override
	{
		auto* Pkt = static_cast<CigiHatHotReqV3*>(Packet);
		if (!Receiver || !Pkt) return;

		FCigiHatHotRequest Req;
		Req.HatHotId     = static_cast<uint16>(Pkt->GetHatHotID());
		Req.ReqType      = static_cast<uint8>(Pkt->GetReqType());
		Req.EntityId     = static_cast<uint16>(Pkt->GetEntityID());
		Req.Lat          = Pkt->GetLat();
		Req.Lon          = Pkt->GetLon();
		Req.Alt          = Pkt->GetAlt();

		Receiver->HatHotReqQueue.Enqueue(Req);
	}
};

class FLosSegReqProcessor : public CigiBaseEventProcessor
{
	FCigiReceiver* Receiver;
public:
	explicit FLosSegReqProcessor(FCigiReceiver* R) : Receiver(R) {}

	void OnPacketReceived(CigiBasePacket* Packet) override
	{
		auto* Pkt = static_cast<CigiLosSegReqV3*>(Packet);
		if (!Receiver || !Pkt) return;

		FCigiLosSegRequest Req;
		Req.LosId    = static_cast<uint16>(Pkt->GetLosID());
		Req.ReqType  = static_cast<uint8>(Pkt->GetReqType());
		Req.EntityId = static_cast<uint16>(Pkt->GetEntityID());
		Req.SrcLat   = Pkt->GetSrcLat();
		Req.SrcLon   = Pkt->GetSrcLon();
		Req.SrcAlt   = Pkt->GetSrcAlt();
		Req.DstLat   = Pkt->GetDstLat();
		Req.DstLon   = Pkt->GetDstLon();
		Req.DstAlt   = Pkt->GetDstAlt();
		// Note: DestEntityIDValid/DestEntityId only available in CIGI 3.2; default to false/0

		Receiver->LosSegReqQueue.Enqueue(Req);
	}
};

class FLosVectReqProcessor : public CigiBaseEventProcessor
{
	FCigiReceiver* Receiver;
public:
	explicit FLosVectReqProcessor(FCigiReceiver* R) : Receiver(R) {}

	void OnPacketReceived(CigiBasePacket* Packet) override
	{
		auto* Pkt = static_cast<CigiLosVectReqV3*>(Packet);
		if (!Receiver || !Pkt) return;

		FCigiLosVectRequest Req;
		Req.LosId        = static_cast<uint16>(Pkt->GetLosID());
		Req.ReqType      = static_cast<uint8>(Pkt->GetReqType());
		Req.EntityId     = static_cast<uint16>(Pkt->GetEntityID());
		Req.VectAz       = Pkt->GetVectAz();
		Req.VectEl       = Pkt->GetVectEl();
		Req.MinRange     = Pkt->GetMinRange();
		Req.MaxRange     = Pkt->GetMaxRange();
		Req.SrcLat       = Pkt->GetSrcLat();
		Req.SrcLon       = Pkt->GetSrcLon();
		Req.SrcAlt       = Pkt->GetSrcAlt();

		Receiver->LosVectReqQueue.Enqueue(Req);
	}
};

// -------------------------------------------------------------------------
// Direct packet parsing for environment packets (opcodes 9, 10, 12).
//
// CCL uses a "hold" mechanism (CigiHoldEnvCtrl) for Celestial and Atmosphere
// packets that merges them before dispatching to event processors.  This makes
// it unreliable for our per-packet event model.  Instead we parse these three
// packet types directly from the raw UDP buffer before CCL sees them.
//
// All values are big-endian on the wire (CIGI 3.3 ICD convention).
// -------------------------------------------------------------------------

class FCigiRawEnvParser
{
public:

static float ReadF32BE(const uint8* P)
{
	union { uint32 I; float F; } U;
	U.I = (uint32(P[0]) << 24) | (uint32(P[1]) << 16) | (uint32(P[2]) << 8) | uint32(P[3]);
	return U.F;
}

static uint16 ReadU16BE(const uint8* P)
{
	return (uint16(P[0]) << 8) | uint16(P[1]);
}

/** Scan raw CIGI datagram for environment packets and enqueue them. */
static void PreParseEnvPackets(const uint8* Buf, int32 Len, FCigiReceiver* Receiver)
{
	int32 Pos = 0;
	while (Pos + 2 <= Len)
	{
		const uint8 PktId   = Buf[Pos];
		const uint8 PktSize = Buf[Pos + 1];
		if (PktSize < 2 || Pos + PktSize > Len) break;

		if (PktId == CIGI_CELESTIAL_CTRL_PACKET_ID_V3 && PktSize >= 16)
		{
			// Celestial Sphere Control (opcode 9, 16 bytes)
			// Byte layout: [0]id [1]size [2]hour [3]minute [4]flags [5]rsvd
			//              [6]month [7]day [8-9]year [10-13]starInt [14-15]rsvd
			const uint8* P = Buf + Pos;
			FCigiCelestialState State;
			State.Hour         = P[2];
			State.Minute       = P[3];
			const uint8 Flags  = P[4];
			State.bEphemerisEn = (Flags & 0x01) != 0;
			State.bSunEn       = (Flags & 0x02) != 0;
			State.bMoonEn      = (Flags & 0x04) != 0;
			State.bStarEn      = (Flags & 0x08) != 0;
			State.bDateVld     = (Flags & 0x10) != 0;
			State.Month        = P[6];
			State.Day          = P[7];
			State.Year         = ReadU16BE(P + 8);
			State.StarInt      = ReadF32BE(P + 10);
			Receiver->CelestialQueue.Enqueue(State);
		}
		else if (PktId == CIGI_ATMOS_CTRL_PACKET_ID_V3 && PktSize >= 32)
		{
			// Atmosphere Control (opcode 10, 32 bytes)
			// [0]id [1]size [2]flags [3]rsvd [4-7]humidity(float)
			// [8-11]airTemp [12-15]visibility [16-19]horizWind
			// [20-23]vertWind [24-27]windDir [28-31]baroPress
			const uint8* P = Buf + Pos;
			FCigiAtmosphereState State;
			State.bAtmosEn    = (P[2] & 0x01) != 0;
			State.Humidity    = ReadF32BE(P + 4);
			State.AirTemp     = ReadF32BE(P + 8);
			State.Visibility  = ReadF32BE(P + 12);
			State.HorizWindSp = ReadF32BE(P + 16);
			State.VertWindSp  = ReadF32BE(P + 20);
			State.WindDir     = ReadF32BE(P + 24);
			State.BaroPress   = ReadF32BE(P + 28);
			Receiver->AtmosphereQueue.Enqueue(State);
		}
		else if (PktId == CIGI_WEATHER_CTRL_PACKET_ID_V3 && PktSize >= 56)
		{
			// Weather Control (opcode 12, 56 bytes)
			// [0]id [1]size [2-3]regionId [4]layerId [5]humidity(u8)
			// [6]flags(weatherEn bit0, cloudType bits4-7)
			// [7]scope(bits0-1) | severity(bits2-4)
			// [8-11]airTemp [12-15]visibilityRng [16-19]scudFreq
			// [20-23]coverage [24-27]baseElev [28-31]thickness
			// [32-35]transition [36-39]horizWind [40-43]vertWind
			// [44-47]windDir [48-51]baroPress [52-55]aerosol
			const uint8* P = Buf + Pos;
			FCigiWeatherState State;
			State.RegionId      = ReadU16BE(P + 2);
			State.LayerId       = P[4];
			const uint8 Flags   = P[6];
			State.bWeatherEn    = (Flags & 0x01) != 0;
			State.CloudType     = (Flags >> 4) & 0x0F;
			const uint8 ScopeSev = P[7];
			State.Scope         = ScopeSev & 0x03;
			State.Severity      = (ScopeSev >> 2) & 0x07;
			State.VisibilityRng = ReadF32BE(P + 12);
			State.Coverage      = ReadF32BE(P + 20);
			State.BaseElev      = ReadF32BE(P + 24);
			State.Thickness     = ReadF32BE(P + 28);
			State.Transition    = ReadF32BE(P + 32);
			State.HorizWindSp   = ReadF32BE(P + 36);
			State.VertWindSp    = ReadF32BE(P + 40);
			State.WindDir       = ReadF32BE(P + 44);
			Receiver->WeatherQueue.Enqueue(State);
		}

		Pos += PktSize;
	}
}

}; // class FCigiRawEnvParser

// -------------------------------------------------------------------------
// Constructor / Destructor
// -------------------------------------------------------------------------

FCigiReceiver::FCigiReceiver(const FCamSimConfig& InConfig)
	: Config(InConfig)
	, bShouldRun(false)
{
}

FCigiReceiver::~FCigiReceiver()
{
	Stop();
}

// -------------------------------------------------------------------------
// Start / Stop
// -------------------------------------------------------------------------

bool FCigiReceiver::Start()
{
	if (!CreateSocket())
	{
		UE_LOG(LogCamSim, Error, TEXT("FCigiReceiver: failed to create UDP socket on port %d"), Config.CigiPort);
		return false;
	}

	bShouldRun = true;
	Thread = FRunnableThread::Create(this, TEXT("CigiReceiverThread"), 128 * 1024,
		TPri_Normal, FPlatformAffinity::GetTaskGraphBackgroundTaskMask());

	UE_LOG(LogCamSim, Log, TEXT("FCigiReceiver: listening on %s:%d (camera entity id=%d)"),
		*Config.CigiBindAddr, Config.CigiPort, Config.CameraEntityId);
	return Thread != nullptr;
}

void FCigiReceiver::Stop()
{
	bShouldRun = false;
	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	if (Socket)
	{
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}
	UE_LOG(LogCamSim, Log, TEXT("FCigiReceiver: stopped"));
}

// -------------------------------------------------------------------------
// FRunnable interface
// -------------------------------------------------------------------------

bool FCigiReceiver::Init()
{
	// Set up CCL IG session (we are the IG, host sends packets).
	// CigiSession owns the IncomingMsg internally; obtain a non-owning pointer via
	// GetIncomingMsgMgr() — do NOT construct CigiIncomingMsg standalone.
	CigiSession = MakeUnique<CigiIGSession>();
	CigiSession->SetCigiVersion(3, 3);

	IncomingMsg = &CigiSession->GetIncomingMsgMgr();
	IncomingMsg->SetReaderCigiVersion(3, 3);

	// Create processor instances and register them
	EntityCtrlProc = MakeUnique<FEntityCtrlProcessor>(this);
	IncomingMsg->RegisterEventProcessor(
		CIGI_ENTITY_CTRL_PACKET_ID_V3, EntityCtrlProc.Get());

	ViewDefProc = MakeUnique<FViewDefProcessor>(this);
	IncomingMsg->RegisterEventProcessor(
		CIGI_VIEW_DEF_PACKET_ID_V3, ViewDefProc.Get());

	RateCtrlProc = MakeUnique<FRateCtrlProcessor>(this);
	IncomingMsg->RegisterEventProcessor(
		CIGI_RATE_CTRL_PACKET_ID_V3, RateCtrlProc.Get());

	ArtPartProc = MakeUnique<FArtPartProcessor>(this);
	IncomingMsg->RegisterEventProcessor(
		CIGI_ART_PART_CTRL_PACKET_ID_V3, ArtPartProc.Get());

	CompCtrlProc = MakeUnique<FCompCtrlProcessor>(this);
	IncomingMsg->RegisterEventProcessor(
		CIGI_COMP_CTRL_PACKET_ID_V3, CompCtrlProc.Get());

	SensorCtrlProc = MakeUnique<FSensorCtrlProcessor>(this);
	IncomingMsg->RegisterEventProcessor(
		CIGI_SENSOR_CTRL_PACKET_ID_V3, SensorCtrlProc.Get());

	ViewCtrlProc = MakeUnique<FViewCtrlProcessor>(this);
	IncomingMsg->RegisterEventProcessor(
		CIGI_VIEW_CTRL_PACKET_ID_V3, ViewCtrlProc.Get());

	HatHotReqProc = MakeUnique<FHatHotReqProcessor>(this);
	IncomingMsg->RegisterEventProcessor(
		CIGI_HAT_HOT_REQ_PACKET_ID_V3, HatHotReqProc.Get());

	LosSegReqProc = MakeUnique<FLosSegReqProcessor>(this);
	IncomingMsg->RegisterEventProcessor(
		CIGI_LOS_SEG_REQ_PACKET_ID_V3, LosSegReqProc.Get());

	LosVectReqProc = MakeUnique<FLosVectReqProcessor>(this);
	IncomingMsg->RegisterEventProcessor(
		CIGI_LOS_VECT_REQ_PACKET_ID_V3, LosVectReqProc.Get());

	// Note: Celestial (9), Atmosphere (10), and Weather (12) packets are parsed
	// directly from the raw buffer in Run() via CigiRawParse::PreParseEnvPackets(),
	// bypassing CCL's CigiHoldEnvCtrl merge mechanism which prevents reliable
	// per-packet event dispatching for these packet types.

	return true;
}

uint32 FCigiReceiver::Run()
{
	static constexpr int32 RecvBufSize = 32768;
	uint8 RecvBuf[RecvBufSize];

	while (bShouldRun)
	{
		int32 BytesRead = 0;
		if (Socket && Socket->Recv(RecvBuf, RecvBufSize, BytesRead) && BytesRead > 0)
		{
			++ReceivedPacketCount;

			// Pre-parse environment packets directly from raw buffer
			// (bypasses CCL's hold mechanism for celestial/atmos/weather)
			FCigiRawEnvParser::PreParseEnvPackets(RecvBuf, BytesRead, this);

			// Feed raw bytes to CCL parser for entity/view/other packets
			try
			{
				IncomingMsg->ProcessIncomingMsg(reinterpret_cast<Cigi_uint8*>(RecvBuf), BytesRead);
			}
			catch (...)
			{
				UE_LOG(LogCamSim, Warning, TEXT("FCigiReceiver: CCL threw exception parsing packet"));
			}
		}
		else
		{
			// Non-blocking socket returned no data — yield briefly
			FPlatformProcess::SleepNoStats(0.001f);
		}
	}

	return 0;
}

void FCigiReceiver::Exit()
{
	// Unregister processors before destroying them
	if (IncomingMsg && EntityCtrlProc)
	{
		IncomingMsg->UnregisterEventProcessor(
			CIGI_ENTITY_CTRL_PACKET_ID_V3, EntityCtrlProc.Get());
	}
	if (IncomingMsg && ViewDefProc)
	{
		IncomingMsg->UnregisterEventProcessor(
			CIGI_VIEW_DEF_PACKET_ID_V3, ViewDefProc.Get());
	}
	if (IncomingMsg && RateCtrlProc)
	{
		IncomingMsg->UnregisterEventProcessor(
			CIGI_RATE_CTRL_PACKET_ID_V3, RateCtrlProc.Get());
	}
	if (IncomingMsg && ArtPartProc)
	{
		IncomingMsg->UnregisterEventProcessor(
			CIGI_ART_PART_CTRL_PACKET_ID_V3, ArtPartProc.Get());
	}
	if (IncomingMsg && CompCtrlProc)
	{
		IncomingMsg->UnregisterEventProcessor(
			CIGI_COMP_CTRL_PACKET_ID_V3, CompCtrlProc.Get());
	}
	if (IncomingMsg && SensorCtrlProc)
	{
		IncomingMsg->UnregisterEventProcessor(
			CIGI_SENSOR_CTRL_PACKET_ID_V3, SensorCtrlProc.Get());
	}
	if (IncomingMsg && ViewCtrlProc)
	{
		IncomingMsg->UnregisterEventProcessor(
			CIGI_VIEW_CTRL_PACKET_ID_V3, ViewCtrlProc.Get());
	}
	if (IncomingMsg && HatHotReqProc)
	{
		IncomingMsg->UnregisterEventProcessor(
			CIGI_HAT_HOT_REQ_PACKET_ID_V3, HatHotReqProc.Get());
	}
	if (IncomingMsg && LosSegReqProc)
	{
		IncomingMsg->UnregisterEventProcessor(
			CIGI_LOS_SEG_REQ_PACKET_ID_V3, LosSegReqProc.Get());
	}
	if (IncomingMsg && LosVectReqProc)
	{
		IncomingMsg->UnregisterEventProcessor(
			CIGI_LOS_VECT_REQ_PACKET_ID_V3, LosVectReqProc.Get());
	}
	EntityCtrlProc.Reset();
	ViewDefProc.Reset();
	RateCtrlProc.Reset();
	ArtPartProc.Reset();
	CompCtrlProc.Reset();
	SensorCtrlProc.Reset();
	ViewCtrlProc.Reset();
	HatHotReqProc.Reset();
	LosSegReqProc.Reset();
	LosVectReqProc.Reset();
	IncomingMsg = nullptr;   // non-owning; session owns and will destroy it
	CigiSession.Reset();
}

// -------------------------------------------------------------------------
// Socket creation
// -------------------------------------------------------------------------

bool FCigiReceiver::CreateSocket()
{
	FIPv4Address BindAddress;
	if (!FIPv4Address::Parse(Config.CigiBindAddr, BindAddress))
	{
		BindAddress = FIPv4Address::Any;
	}

	Socket = FUdpSocketBuilder(TEXT("CigiSocket"))
		.AsNonBlocking()
		.AsReusable()
		.BoundToAddress(BindAddress)
		.BoundToPort(Config.CigiPort)
		.WithReceiveBufferSize(256 * 1024)
		.Build();

	return Socket != nullptr;
}
