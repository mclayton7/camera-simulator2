// Copyright CamSim Contributors. All Rights Reserved.

#include "CIGI/CigiReceiver.h"
#include "CamSimTest.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Common/UdpSocketBuilder.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "HAL/PlatformFileManager.h"     // Phase 12E: CIGI recording/playback

// CCL headers (static lib, drop-in) — wrapped to suppress third-party warnings
THIRD_PARTY_INCLUDES_START
#include "cigicl/CigiIGSession.h"
#include "cigicl/CigiIncomingMsg.h"
#include "cigicl/CigiBaseEventProcessor.h"
#include "cigicl/CigiEntityCtrlV3.h"
#include "cigicl/CigiEntityCtrlV3_3.h"
#include "cigicl/CigiViewDefV3.h"
#include "cigicl/CigiRateCtrlV3.h"      // opcode 8
#include "cigicl/CigiRateCtrlV3_2.h"
#include "cigicl/CigiArtPartCtrlV3.h"   // opcode 6
#include "cigicl/CigiCompCtrlV3.h"      // opcode 4
#include "cigicl/CigiCompCtrlV3_3.h"
#include "cigicl/CigiSensorCtrlV3.h"    // opcode 17
#include "cigicl/CigiViewCtrlV3.h"      // opcode 16
#include "cigicl/CigiHatHotReqV3.h"     // opcode 24
#include "cigicl/CigiHatHotReqV3_2.h"
#include "cigicl/CigiLosSegReqV3.h"     // opcode 25
#include "cigicl/CigiLosSegReqV3_2.h"
#include "cigicl/CigiLosVectReqV3.h"    // opcode 26
#include "cigicl/CigiLosVectReqV3_2.h"
#include "cigicl/CigiIGCtrlV3.h"       // opcode 1 (IG Control — host frame counter)
#include "cigicl/CigiIGCtrlV3_2.h"
#include "cigicl/CigiIGCtrlV3_3.h"
#include "cigicl/CigiWaveCtrlV3.h"     // opcode 14 (Wave Control — ocean waves)
#include "cigicl/CigiConfClampEntityCtrlV3.h" // opcode 3  (Conformal Clamped Entity)
#include "cigicl/CigiCollDetSegDefV3.h"       // opcode 7  (Collision Detection Segment Def)
#include "cigicl/CigiMaritimeSurfaceCtrlV3.h" // opcode 13 (Maritime Surface Conditions)
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

// CCL builds a different class for the same opcode depending on the host's CIGI
// minor version (e.g. CigiRateCtrlV3 for 3.0/3.1, CigiRateCtrlV3_2 for 3.2+) and
// stamps the packet with that minor version. The accessors live on those
// classes, not the CigiBase* class, so visit the packet as the class CCL built.
// (UE builds without RTTI, so dynamic_cast is not an option.)
template <typename TV3, typename TV3x, typename FVisitor>
static void VisitVersioned(CigiBasePacket* Packet, int V3xMinorVersion, FVisitor&& Visit)
{
	if (Packet->GetMinorVersion() >= V3xMinorVersion)
	{
		Visit(*static_cast<TV3x*>(Packet));
	}
	else
	{
		Visit(*static_cast<TV3*>(Packet));
	}
}

template <typename TPacket, typename TClass>
inline constexpr bool IsPacketClass = std::is_same_v<std::decay_t<TPacket>, TClass>;

class FEntityCtrlProcessor : public CigiBaseEventProcessor
{
	FCigiReceiver* Receiver;
public:
	explicit FEntityCtrlProcessor(FCigiReceiver* R) : Receiver(R) {}

	void OnPacketReceived(CigiBasePacket* Packet) override
	{
		if (!Receiver || !Packet) return;

		FCigiEntityState State;
		VisitVersioned<CigiEntityCtrlV3, CigiEntityCtrlV3_3>(Packet, 3, [&](auto& Pkt)
		{
			State.EntityId    = static_cast<uint16>(Pkt.GetEntityID());
			State.bAttached   = (Pkt.GetAttachState() == CigiBaseEntityCtrl::Attach);
			State.ParentId    = static_cast<uint16>(Pkt.GetParentID());
			State.EntityState = static_cast<uint8>(Pkt.GetEntityState());
			State.EntityType  = static_cast<uint16>(Pkt.GetEntityType());
			State.Latitude    = Pkt.GetLat();
			State.Longitude   = Pkt.GetLon();
			State.Altitude    = static_cast<float>(Pkt.GetAlt());
			State.Yaw         = static_cast<float>(Pkt.GetYaw());
			State.Pitch       = static_cast<float>(Pkt.GetPitch());
			State.Roll        = static_cast<float>(Pkt.GetRoll());
		});
		State.HostTimeSec = Receiver->HostClock.Now();
		// CIGI V3 EntityCtrl has no Kind/Domain/Category fields; leave at defaults (0).

		// Route by entity ID: camera entity → this datagram's camera frame; others → EntityStateQueue
		if (State.EntityId == static_cast<uint16>(Receiver->Config.CameraEntityId))
		{
			Receiver->PendingCameraFrame.bHasPose = true;  // last one in the datagram wins
			Receiver->PendingCameraFrame.Pose     = State;
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

		Receiver->PendingCameraFrame.ViewDefinitions.Add(View);
	}
};

class FRateCtrlProcessor : public CigiBaseEventProcessor
{
	FCigiReceiver* Receiver;
public:
	explicit FRateCtrlProcessor(FCigiReceiver* R) : Receiver(R) {}

	void OnPacketReceived(CigiBasePacket* Packet) override
	{
		if (!Receiver || !Packet) return;

		FCigiRateControl Rate;
		VisitVersioned<CigiRateCtrlV3, CigiRateCtrlV3_2>(Packet, 2, [&Rate](auto& Pkt)
		{
			Rate.EntityId        = static_cast<uint16>(Pkt.GetEntityID());
			Rate.ArtPartId       = static_cast<uint8>(Pkt.GetArtPartID());
			Rate.bApplyToArtPart = Pkt.GetApplyToArtPart();
			Rate.XRate           = static_cast<float>(Pkt.GetXRate());
			Rate.YRate           = static_cast<float>(Pkt.GetYRate());
			Rate.ZRate           = static_cast<float>(Pkt.GetZRate());
			Rate.RollRate        = static_cast<float>(Pkt.GetRollRate());
			Rate.PitchRate       = static_cast<float>(Pkt.GetPitchRate());
			Rate.YawRate         = static_cast<float>(Pkt.GetYawRate());
			// CIGI 3.0/3.1 has no Coordinate System field: rates are body-frame.
			if constexpr (IsPacketClass<decltype(Pkt), CigiRateCtrlV3_2>)
			{
				Rate.bLocalFrame        = (Pkt.GetCoordSys() == CigiBaseRateCtrl::Local);
				Rate.bAngularLocalFrame = Rate.bLocalFrame;
			}
		});

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

		// Route to the camera gimbal (this datagram's camera frame) or the entity queue
		if (Art.EntityId == static_cast<uint16>(Receiver->Config.CameraEntityId))
		{
			Receiver->PendingCameraFrame.ArtParts.Add(Art);
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
		if (!Receiver || !Packet) return;

		FCigiComponentControl Comp;
		VisitVersioned<CigiCompCtrlV3, CigiCompCtrlV3_3>(Packet, 3, [&Comp](auto& Pkt)
		{
			Comp.EntityId  = static_cast<uint16>(Pkt.GetInstanceID());  // CCL uses InstanceID
			Comp.CompId    = static_cast<uint16>(Pkt.GetCompID());
			Comp.CompClass = static_cast<uint8>(Pkt.GetCompClassV3());
			Comp.CompState = static_cast<uint8>(Pkt.GetCompState());
		});

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

		Receiver->PendingCameraFrame.SensorControls.Add(Sensor);
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

		Receiver->PendingCameraFrame.ViewControls.Add(View);
	}
};

class FHatHotReqProcessor : public CigiBaseEventProcessor
{
	FCigiReceiver* Receiver;
public:
	explicit FHatHotReqProcessor(FCigiReceiver* R) : Receiver(R) {}

	void OnPacketReceived(CigiBasePacket* Packet) override
	{
		if (!Receiver || !Packet) return;

		FCigiHatHotRequest Req;
		VisitVersioned<CigiHatHotReqV3, CigiHatHotReqV3_2>(Packet, 2, [&Req](auto& Pkt)
		{
			Req.HatHotId     = static_cast<uint16>(Pkt.GetHatHotID());
			Req.ReqType      = static_cast<uint8>(Pkt.GetReqType());
			Req.EntityId     = static_cast<uint16>(Pkt.GetEntityID());
			Req.bEntityRelative = (Pkt.GetSrcCoordSys() == CigiBaseHatHotReq::Entity);
			Req.Lat          = Pkt.GetLat();
			Req.Lon          = Pkt.GetLon();
			Req.Alt          = Pkt.GetAlt();
		});

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
		if (!Receiver || !Packet) return;

		FCigiLosSegRequest Req;
		VisitVersioned<CigiLosSegReqV3, CigiLosSegReqV3_2>(Packet, 2, [&Req](auto& Pkt)
		{
			Req.LosId    = static_cast<uint16>(Pkt.GetLosID());
			Req.ReqType  = static_cast<uint8>(Pkt.GetReqType());
			Req.EntityId = static_cast<uint16>(Pkt.GetEntityID());
			Req.bSrcEntityRelative = (Pkt.GetSrcCoordSys() == CigiBaseLosSegReq::Entity);
			Req.bDstEntityRelative = (Pkt.GetDstCoordSys() == CigiBaseLosSegReq::Entity);
			Req.SrcLat   = Pkt.GetSrcLat();
			Req.SrcLon   = Pkt.GetSrcLon();
			Req.SrcAlt   = Pkt.GetSrcAlt();
			Req.DstLat   = Pkt.GetDstLat();
			Req.DstLon   = Pkt.GetDstLon();
			Req.DstAlt   = Pkt.GetDstAlt();
			// Destination Entity ID was added in CIGI 3.2.
			if constexpr (IsPacketClass<decltype(Pkt), CigiLosSegReqV3_2>)
			{
				Req.bDestEntityIDValid = Pkt.GetDestEntityIDValid();
				Req.DestEntityId       = static_cast<uint16>(Pkt.GetDestEntityID());
				Req.bResponseEntityCs  = (Pkt.GetResponseCoordSys() == CigiBaseLosSegReq::Entity);
			}
		});

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
		if (!Receiver || !Packet) return;

		FCigiLosVectRequest Req;
		VisitVersioned<CigiLosVectReqV3, CigiLosVectReqV3_2>(Packet, 2, [&Req](auto& Pkt)
		{
			Req.LosId        = static_cast<uint16>(Pkt.GetLosID());
			Req.ReqType      = static_cast<uint8>(Pkt.GetReqType());
			Req.EntityId     = static_cast<uint16>(Pkt.GetEntityID());
			Req.bEntityRelative = (Pkt.GetSrcCoordSys() == CigiBaseLosVectReq::Entity);
			Req.VectAz       = Pkt.GetVectAz();
			Req.VectEl       = Pkt.GetVectEl();
			Req.MinRange     = Pkt.GetMinRange();
			Req.MaxRange     = Pkt.GetMaxRange();
			Req.SrcLat       = Pkt.GetSrcLat();
			Req.SrcLon       = Pkt.GetSrcLon();
			Req.SrcAlt       = Pkt.GetSrcAlt();
			if constexpr (IsPacketClass<decltype(Pkt), CigiLosVectReqV3_2>)
			{
				Req.bResponseEntityCs = (Pkt.GetResponseCoordSys() == CigiBaseLosVectReq::Entity);
			}
		});

		Receiver->LosVectReqQueue.Enqueue(Req);
	}
};

class FIGCtrlProcessor : public CigiBaseEventProcessor
{
	FCigiReceiver* Receiver;
public:
	explicit FIGCtrlProcessor(FCigiReceiver* R) : Receiver(R) {}

	void OnPacketReceived(CigiBasePacket* Packet) override
	{
		if (!Receiver || !Packet) return;

		auto Apply = [this](auto& Pkt)
		{
			Receiver->LastHostFrameCntr.Store(static_cast<uint32>(Pkt.GetFrameCntr()));
			Receiver->HostClock.OnIgControl(Pkt.GetTimeStampValid(),
				static_cast<uint32>(Pkt.GetTimeStamp()));
		};
		if (Packet->GetMinorVersion() >= 3)
		{
			Apply(*static_cast<CigiIGCtrlV3_3*>(Packet));
		}
		else
		{
			VisitVersioned<CigiIGCtrlV3, CigiIGCtrlV3_2>(Packet, 2, Apply);
		}
	}
};

// -------------------------------------------------------------------------
// Direct packet parsing for environment packets (opcodes 9, 10, 12) and the
// user-defined Platform Kinematics packet (opcode 201; CCL skips user-defined
// opcodes it has no handler for).
//
// CCL uses a "hold" mechanism (CigiHoldEnvCtrl) for Celestial and Atmosphere
// packets that merges them before dispatching to event processors.  This makes
// it unreliable for our per-packet event model.  Instead we parse these three
// packet types directly from the raw UDP buffer before CCL sees them.
//
// Layouts match CCL's CigiCelestialCtrl / CigiAtmosCtrl / CigiWeatherCtrlV3
// Pack(). Multi-byte fields are in the host's byte order, which the IG
// Control packet at the start of each message declares with its Byte Swap
// Magic (0x8000 in the sender's order): CCL hosts send native order, which is
// little-endian on x86 and ARM.
// -------------------------------------------------------------------------

class FCigiRawEnvParser
{
public:

struct FReader
{
	bool bBigEndian = true;

	uint16 U16(const uint8* P) const
	{
		return bBigEndian ? uint16((P[0] << 8) | P[1]) : uint16((P[1] << 8) | P[0]);
	}
	uint32 U32(const uint8* P) const
	{
		return bBigEndian
			? (uint32(P[0]) << 24) | (uint32(P[1]) << 16) | (uint32(P[2]) << 8) | uint32(P[3])
			: (uint32(P[3]) << 24) | (uint32(P[2]) << 16) | (uint32(P[1]) << 8) | uint32(P[0]);
	}
	float F32(const uint8* P) const
	{
		const uint32 Bits = U32(P);
		float F;
		FMemory::Memcpy(&F, &Bits, sizeof(F));
		return F;
	}
	double F64(const uint8* P) const
	{
		const uint64 Hi = U32(bBigEndian ? P : P + 4);
		const uint64 Lo = U32(bBigEndian ? P + 4 : P);
		const uint64 Bits = (Hi << 32) | Lo;
		double D;
		FMemory::Memcpy(&D, &Bits, sizeof(D));
		return D;
	}
};

/** Byte order declared by the message's leading IG Control (big-endian if absent). */
static FReader ReaderForMessage(const uint8* Buf, int32 Len)
{
	FReader R;
	if (Len >= 8 && Buf[0] == 1)  // IG Control, Byte Swap Magic at bytes 6-7
	{
		R.bBigEndian = !(Buf[6] == 0x00 && Buf[7] == 0x80);
	}
	return R;
}

/** Scan raw CIGI datagram for environment packets and enqueue them. */
static void PreParseEnvPackets(const uint8* Buf, int32 Len, FCigiReceiver* Receiver)
{
	const FReader R = ReaderForMessage(Buf, Len);
	int32 Pos = 0;
	while (Pos + 2 <= Len)
	{
		const uint8 PktId   = Buf[Pos];
		const uint8 PktSize = Buf[Pos + 1];
		if (PktSize < 2 || Pos + PktSize > Len) break;
		const uint8* P = Buf + Pos;

		if (PktId == CIGI_CELESTIAL_CTRL_PACKET_ID_V3 && PktSize >= 16)
		{
			// Celestial Sphere Control (opcode 9, 16 bytes)
			// [2]hour [3]minute [4]flags [5-7]reserved
			// [8-11]date uint32 MMDDYYYY [12-15]star intensity float
			FCigiCelestialState State;
			State.Hour         = FMath::Clamp((int32)P[2], 0, 23);
			State.Minute       = FMath::Clamp((int32)P[3], 0, 59);
			const uint8 Flags  = P[4];
			State.bEphemerisEn = (Flags & 0x01) != 0;
			State.bSunEn       = (Flags & 0x02) != 0;
			State.bMoonEn      = (Flags & 0x04) != 0;
			State.bStarEn      = (Flags & 0x08) != 0;
			State.bDateVld     = (Flags & 0x10) != 0;
			const uint32 Date  = R.U32(P + 8);
			State.Month        = static_cast<uint8>(Date / 1000000);
			State.Day          = static_cast<uint8>((Date / 10000) % 100);
			State.Year         = static_cast<uint16>(Date % 10000);
			State.StarInt      = R.F32(P + 12);
			Receiver->CelestialQueue.Enqueue(State);
		}
		else if (PktId == CIGI_ATMOS_CTRL_PACKET_ID_V3 && PktSize >= 32)
		{
			// Atmosphere Control (opcode 10, 32 bytes)
			// [2]flags [3]humidity uint8 % [4]airTemp [8]visibility
			// [12]horizWind [16]vertWind [20]windDir [24]baroPress [28-31]reserved
			FCigiAtmosphereState State;
			State.bAtmosEn    = (P[2] & 0x01) != 0;
			State.Humidity    = P[3];
			State.AirTemp     = R.F32(P + 4);
			State.Visibility  = R.F32(P + 8);
			State.HorizWindSp = R.F32(P + 12);
			State.VertWindSp  = R.F32(P + 16);
			State.WindDir     = R.F32(P + 20);
			State.BaroPress   = R.F32(P + 24);
			Receiver->AtmosphereQueue.Enqueue(State);
		}
		else if (PktId == CIGI_WEATHER_CTRL_PACKET_ID_V3 && PktSize >= 56)
		{
			// Weather Control (opcode 12, 56 bytes)
			// [2-3]regionId/entityId [4]layerId [5]humidity(u8)
			// [6]flags(weatherEn bit0, cloudType bits4-7)
			// [7]scope(bits0-1) | severity(bits2-4)
			// [8-11]airTemp [12-15]visibilityRng [16-19]scudFreq
			// [20-23]coverage [24-27]baseElev [28-31]thickness
			// [32-35]transition [36-39]horizWind [40-43]vertWind
			// [44-47]windDir [48-51]baroPress [52-55]aerosol
			FCigiWeatherState State;
			State.RegionId      = R.U16(P + 2);
			State.LayerId       = P[4];
			const uint8 Flags   = P[6];
			State.bWeatherEn    = (Flags & 0x01) != 0;
			State.CloudType     = (Flags >> 4) & 0x0F;
			const uint8 ScopeSev = P[7];
			State.Scope         = ScopeSev & 0x03;
			State.Severity      = (ScopeSev >> 2) & 0x07;
			State.VisibilityRng = R.F32(P + 12);
			State.Coverage      = R.F32(P + 20);
			State.BaseElev      = R.F32(P + 24);
			State.Thickness     = R.F32(P + 28);
			State.Transition    = R.F32(P + 32);
			State.HorizWindSp   = R.F32(P + 36);
			State.VertWindSp    = R.F32(P + 40);
			State.WindDir       = R.F32(P + 44);
			Receiver->WeatherQueue.Enqueue(State);
		}
		else if (PktId == FCigiPlatformKinematics::Opcode && PktSize >= FCigiPlatformKinematics::PacketSize)
		{
			// Platform Kinematics (user-defined opcode 201, 48 bytes, hitl/PROTOCOL.md section 2)
			// [2-3]entityId [4]flags [5-7]reserved [8]TAS [12]IAS [16]magHeading
			// [20]velN [24]velE [28]velD (float) [32-39]sampleUtc (double) [40-47]reserved
			FCigiPlatformKinematics K;
			K.EntityId             = R.U16(P + 2);
			K.Flags                = P[4];
			K.TrueAirspeedMps      = R.F32(P + 8);
			K.IndicatedAirspeedMps = R.F32(P + 12);
			K.MagneticHeadingDeg   = R.F32(P + 16);
			K.VelNorthMps          = R.F32(P + 20);
			K.VelEastMps           = R.F32(P + 24);
			K.VelDownMps           = R.F32(P + 28);
			K.SampleUtcSec         = R.F64(P + 32);
			// Only the camera platform carries KLV kinematics; other IDs are ignored.
			if (K.EntityId == static_cast<uint16>(Receiver->Config.CameraEntityId))
			{
				Receiver->PendingCameraFrame.bHasKinematics = true;
				Receiver->PendingCameraFrame.Kinematics     = K;
			}
		}

		Pos += PktSize;
	}
}

}; // class FCigiRawEnvParser

// ---------------------------------------------------------------------------
// Wave Control (opcode 14)
// ---------------------------------------------------------------------------
class FWaveCtrlProcessor : public CigiBaseEventProcessor
{
	FCigiReceiver* Receiver;
public:
	explicit FWaveCtrlProcessor(FCigiReceiver* R) : Receiver(R) {}

	void OnPacketReceived(CigiBasePacket* Packet) override
	{
		auto* Pkt = static_cast<CigiWaveCtrlV3*>(Packet);
		if (!Receiver || !Pkt) return;

		FCigiWaveState State;
		State.EntityRgnId    = static_cast<uint16>(Pkt->GetEntityRgnID());
		State.WaveID         = static_cast<uint8>(Pkt->GetWaveID());
		State.bEnabled       = Pkt->GetWaveEn();
		State.Scope          = static_cast<uint8>(Pkt->GetScope());
		State.Breaker        = static_cast<uint8>(Pkt->GetBreaker());
		State.WaveHtM        = static_cast<float>(Pkt->GetWaveHt());
		State.WaveLenM       = static_cast<float>(Pkt->GetWaveLen());
		State.PeriodS        = static_cast<float>(Pkt->GetPeriod());
		State.DirectionDeg   = static_cast<float>(Pkt->GetDirection());
		State.PhaseOffsetDeg = static_cast<float>(Pkt->GetPhaseOff());

		Receiver->WaveStateQueue.Enqueue(State);
	}
};

// ---------------------------------------------------------------------------
// Phase 26D: Conformal Clamped Entity Control (opcode 3)
// ---------------------------------------------------------------------------
class FConfClampProcessor : public CigiBaseEventProcessor
{
	FCigiReceiver* Receiver;
public:
	explicit FConfClampProcessor(FCigiReceiver* R) : Receiver(R) {}

	void OnPacketReceived(CigiBasePacket* Packet) override
	{
		auto* Pkt = static_cast<CigiConfClampEntityCtrlV3*>(Packet);
		if (!Receiver || !Pkt) return;

		FCigiConfClampEntityState State;
		State.EntityId  = static_cast<uint16>(Pkt->GetEntityID());
		State.Latitude  = Pkt->GetLat();
		State.Longitude = Pkt->GetLon();
		State.Yaw       = static_cast<float>(Pkt->GetYaw());

		Receiver->ConfClampQueue.Enqueue(State);
	}
};

// ---------------------------------------------------------------------------
// Phase 26D: Collision Detection Segment Definition (opcode 7) — stub
// ---------------------------------------------------------------------------
class FCollisionDetSegProcessor : public CigiBaseEventProcessor
{
	FCigiReceiver* Receiver;
public:
	explicit FCollisionDetSegProcessor(FCigiReceiver* R) : Receiver(R) {}

	void OnPacketReceived(CigiBasePacket* Packet) override
	{
		auto* Pkt = static_cast<CigiCollDetSegDefV3*>(Packet);
		if (!Receiver || !Pkt) return;

		UE_LOG(LogCamSim, Verbose,
			TEXT("FCigiReceiver: CollisionDetSegDef entity=%d seg=%d enabled=%d (stub, not processed)"),
			Pkt->GetEntityID(), Pkt->GetSegmentID(), Pkt->GetSegmentEn() ? 1 : 0);
	}
};

// ---------------------------------------------------------------------------
// Phase 26D: Maritime Surface Conditions Control (opcode 13)
// ---------------------------------------------------------------------------
class FMaritimeSurfaceProcessor : public CigiBaseEventProcessor
{
	FCigiReceiver* Receiver;
public:
	explicit FMaritimeSurfaceProcessor(FCigiReceiver* R) : Receiver(R) {}

	void OnPacketReceived(CigiBasePacket* Packet) override
	{
		auto* Pkt = static_cast<CigiMaritimeSurfaceCtrlV3*>(Packet);
		if (!Receiver || !Pkt) return;

		FCigiMaritimeSurfaceState State;
		State.EntityRgnId    = static_cast<uint16>(Pkt->GetEntityRgnID());
		State.bSurfaceCondEn = Pkt->GetSurfaceCondEn();
		State.bWhitecapEn    = Pkt->GetWhitecapEn();
		State.Scope          = static_cast<uint8>(Pkt->GetScope());
		State.SurfaceHeight  = static_cast<float>(Pkt->GetSurfaceHeight());
		State.WaterTemp      = static_cast<float>(Pkt->GetWaterTemp());
		State.Clarity        = static_cast<float>(Pkt->GetClarity());

		Receiver->MaritimeSurfaceQueue.Enqueue(State);
	}
};

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
	// Phase 12E: Playback mode — read from file instead of UDP
	bPlaybackMode = !Config.Recording.CigiPlaybackPath.IsEmpty();

	if (!bPlaybackMode)
	{
		if (!CreateSocket())
		{
			UE_LOG(LogCamSim, Error, TEXT("FCigiReceiver: failed to create UDP socket on port %d"), Config.CigiPort);
			return false;
		}
	}
	else
	{
		UE_LOG(LogCamSim, Log, TEXT("FCigiReceiver: playback mode from %s"), *Config.Recording.CigiPlaybackPath);
	}

	// Phase 12E: Open recording file if configured
	if (!Config.Recording.CigiRecordPath.IsEmpty() && !bPlaybackMode)
	{
		RecordFileHandle = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Config.Recording.CigiRecordPath);
		if (RecordFileHandle)
		{
			UE_LOG(LogCamSim, Log, TEXT("FCigiReceiver: recording CIGI input to %s"), *Config.Recording.CigiRecordPath);
		}
		else
		{
			UE_LOG(LogCamSim, Warning, TEXT("FCigiReceiver: failed to open recording file %s"), *Config.Recording.CigiRecordPath);
		}
	}

	bShouldRun = true;
	if (!ShutdownEvent)
	{
		// Auto-reset: one Wait() returns per Trigger(), so the receiver doesn't
		// need to manually Reset() the event between poll iterations.
		ShutdownEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/false);
	}
	Thread.Reset(FRunnableThread::Create(this, TEXT("CigiReceiverThread"), 128 * 1024,
		TPri_Normal, FPlatformAffinity::GetTaskGraphBackgroundTaskMask()));

	UE_LOG(LogCamSim, Log, TEXT("FCigiReceiver: %s (camera entity id=%d)"),
		bPlaybackMode ? TEXT("playback mode") : *FString::Printf(TEXT("listening on %s:%d"), *Config.CigiBindAddr, Config.CigiPort),
		Config.CameraEntityId);
	return Thread.IsValid();
}

void FCigiReceiver::Stop()
{
	bShouldRun = false;
	// Wake the receiver from its 1 ms recv poll immediately — without this,
	// shutdown latency is capped at the poll interval per waiting thread.
	if (ShutdownEvent) ShutdownEvent->Trigger();
	if (Thread.IsValid())
	{
		Thread->WaitForCompletion();
		Thread.Reset();
	}
	if (ShutdownEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(ShutdownEvent);
		ShutdownEvent = nullptr;
	}
	if (Socket)
	{
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}
	// Phase 12E: close recording file (Phase 2: final flush to ensure
	// the tail of the file is durable before delete).
	if (RecordFileHandle)
	{
		RecordFileHandle->Flush();
		delete RecordFileHandle;
		RecordFileHandle = nullptr;
		UE_LOG(LogCamSim, Log, TEXT("FCigiReceiver: recording file closed"));
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

	IGCtrlProc = MakeUnique<FIGCtrlProcessor>(this);
	IncomingMsg->RegisterEventProcessor(
		CIGI_IG_CTRL_PACKET_ID_V3, IGCtrlProc.Get());

	WaveCtrlProc = MakeUnique<FWaveCtrlProcessor>(this);
	IncomingMsg->RegisterEventProcessor(CIGI_WAVE_CTRL_PACKET_ID_V3,
	                                     WaveCtrlProc.Get());

	// Phase 26D: Conformal Clamped Entity Control (opcode 3)
	ConfClampProc = MakeUnique<FConfClampProcessor>(this);
	IncomingMsg->RegisterEventProcessor(
		CIGI_CONF_CLAMP_ENTITY_CTRL_PACKET_ID_V3, ConfClampProc.Get());

	// Phase 26D: Collision Detection Segment Definition (opcode 7) — stub
	CollisionDetSegProc = MakeUnique<FCollisionDetSegProcessor>(this);
	IncomingMsg->RegisterEventProcessor(
		CIGI_COLL_DET_SEG_DEF_PACKET_ID_V3, CollisionDetSegProc.Get());

	// Phase 26D: Maritime Surface Conditions Control (opcode 13)
	MaritimeSurfaceProc = MakeUnique<FMaritimeSurfaceProcessor>(this);
	IncomingMsg->RegisterEventProcessor(
		CIGI_MARITIME_SURFACE_CTRL_PACKET_ID_V3, MaritimeSurfaceProc.Get());

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

	// Phase 12E: Playback mode — read from recorded file
	if (bPlaybackMode)
	{
		IFileHandle* PlaybackFile = FPlatformFileManager::Get().GetPlatformFile()
			.OpenRead(*Config.Recording.CigiPlaybackPath);
		if (!PlaybackFile)
		{
			UE_LOG(LogCamSim, Error, TEXT("FCigiReceiver: cannot open playback file %s"),
				*Config.Recording.CigiPlaybackPath);
			return 1;
		}

		uint64 PrevTimestamp = 0;
		while (bShouldRun)
		{
			// Record format: [uint64 timestamp_us] [uint32 length] [bytes...]
			uint64 Timestamp = 0;
			uint32 Length = 0;
			if (!PlaybackFile->Read(reinterpret_cast<uint8*>(&Timestamp), sizeof(Timestamp))) break;
			if (!PlaybackFile->Read(reinterpret_cast<uint8*>(&Length), sizeof(Length))) break;
			if (Length == 0 || Length > RecvBufSize) break;
			if (!PlaybackFile->Read(RecvBuf, Length)) break;

			// Pace playback using inter-packet timing
			if (PrevTimestamp > 0 && Timestamp > PrevTimestamp)
			{
				const double DelayMs = static_cast<double>(Timestamp - PrevTimestamp) / 1000.0;
				if (DelayMs > 0.0 && DelayMs < 5000.0)
				{
					FPlatformProcess::SleepNoStats(static_cast<float>(DelayMs / 1000.0));
				}
			}
			PrevTimestamp = Timestamp;

			++ReceivedPacketCount;
			ProcessDatagram(RecvBuf, static_cast<int32>(Length));
		}
		delete PlaybackFile;
		UE_LOG(LogCamSim, Log, TEXT("FCigiReceiver: playback complete"));
		return 0;
	}

	// Normal UDP receive mode.
	// Phase 2: flush the recording file every 100 packets instead of every
	// packet (60 fsync/sec at ~60 Hz CIGI → ~0.6/sec). Crash recovery loses
	// up to ~1.7 s of recording, acceptable for a diagnostic feature.
	int32 RecordWriteCount = 0;
	while (bShouldRun)
	{
		int32 BytesRead = 0;
		if (Socket && Socket->Recv(RecvBuf, RecvBufSize, BytesRead) && BytesRead > 0)
		{
			++ReceivedPacketCount;

			// Phase 12E: Record raw datagram to file
			if (RecordFileHandle)
			{
				const uint64 Ts = static_cast<uint64>(FPlatformTime::Seconds() * 1000000.0);
				const uint32 Len = static_cast<uint32>(BytesRead);
				RecordFileHandle->Write(reinterpret_cast<const uint8*>(&Ts), sizeof(Ts));
				RecordFileHandle->Write(reinterpret_cast<const uint8*>(&Len), sizeof(Len));
				RecordFileHandle->Write(RecvBuf, BytesRead);
				if (((++RecordWriteCount) % 100) == 0)
				{
					RecordFileHandle->Flush();
				}
			}

			ProcessDatagram(RecvBuf, BytesRead);
		}
		else
		{
			// Non-blocking socket returned no data — wait on the shutdown event
			// with a 1 ms timeout so we wake immediately on Stop() instead of
			// serving out the full sleep interval.
			if (ShutdownEvent) ShutdownEvent->Wait(1);
			else FPlatformProcess::SleepNoStats(0.001f);
		}
	}

	return 0;
}

void FCigiReceiver::ProcessDatagram(uint8* Buf, int32 Len)
{
	PendingCameraFrame.Reset();

	// Pre-parse environment and user-defined packets directly from the raw
	// buffer (bypasses CCL's hold mechanism for celestial/atmos/weather).
	HostClock.BeginMessage(FPlatformTime::Seconds());
	FCigiRawEnvParser::PreParseEnvPackets(Buf, Len, this);

	// Feed raw bytes to CCL parser for entity/view/other packets
	// Phase 13C: catch typed exceptions for better diagnostics
	try
	{
		IncomingMsg->ProcessIncomingMsg(reinterpret_cast<Cigi_uint8*>(Buf), Len);
	}
	catch (const std::exception& Ex)
	{
		UE_LOG(LogCamSim, Warning,
			TEXT("FCigiReceiver: CCL exception parsing packet (%d bytes): %hs"), Len, Ex.what());
	}
	catch (...)
	{
		UE_LOG(LogCamSim, Warning,
			TEXT("FCigiReceiver: CCL threw unknown exception parsing packet (%d bytes)"), Len);
	}

	// Publish the camera packets together (HITL.md gap 2). A sample time only
	// describes the pose of its own datagram.
	if (PendingCameraFrame.bHasKinematics && !PendingCameraFrame.bHasPose)
	{
		PendingCameraFrame.Kinematics.Flags &= ~FCigiPlatformKinematics::FlagSampleTime;
	}
	if (!PendingCameraFrame.IsEmpty())
	{
		CameraFrameQueue.Enqueue(MoveTemp(PendingCameraFrame));
		PendingCameraFrame = FCigiCameraFrame();
	}
}

void FCigiReceiver::Exit()
{
	// Unregister each processor and reset its unique ptr in one step.
	auto Unreg = [&](auto& Proc, int PacketId)
	{
		if (IncomingMsg && Proc)
		{
			IncomingMsg->UnregisterEventProcessor(PacketId, Proc.Get());
			Proc.Reset();
		}
	};

	Unreg(EntityCtrlProc,  CIGI_ENTITY_CTRL_PACKET_ID_V3);
	Unreg(ViewDefProc,     CIGI_VIEW_DEF_PACKET_ID_V3);
	Unreg(RateCtrlProc,    CIGI_RATE_CTRL_PACKET_ID_V3);
	Unreg(ArtPartProc,     CIGI_ART_PART_CTRL_PACKET_ID_V3);
	Unreg(CompCtrlProc,    CIGI_COMP_CTRL_PACKET_ID_V3);
	Unreg(SensorCtrlProc,  CIGI_SENSOR_CTRL_PACKET_ID_V3);
	Unreg(ViewCtrlProc,    CIGI_VIEW_CTRL_PACKET_ID_V3);
	Unreg(HatHotReqProc,   CIGI_HAT_HOT_REQ_PACKET_ID_V3);
	Unreg(LosSegReqProc,   CIGI_LOS_SEG_REQ_PACKET_ID_V3);
	Unreg(LosVectReqProc,  CIGI_LOS_VECT_REQ_PACKET_ID_V3);
	Unreg(IGCtrlProc,      CIGI_IG_CTRL_PACKET_ID_V3);
	Unreg(WaveCtrlProc,   CIGI_WAVE_CTRL_PACKET_ID_V3);
	Unreg(ConfClampProc,      CIGI_CONF_CLAMP_ENTITY_CTRL_PACKET_ID_V3);
	Unreg(CollisionDetSegProc, CIGI_COLL_DET_SEG_DEF_PACKET_ID_V3);
	Unreg(MaritimeSurfaceProc, CIGI_MARITIME_SURFACE_CTRL_PACKET_ID_V3);

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

uint64 FCigiReceiver::GetTotalDropCount() const
{
	return CameraFrameQueue.GetDropCount()
	     + EntityStateQueue.GetDropCount()
	     + CelestialQueue.GetDropCount()
	     + AtmosphereQueue.GetDropCount()
	     + WeatherQueue.GetDropCount()
	     + RateCtrlQueue.GetDropCount()
	     + ArtPartQueue.GetDropCount()
	     + CompCtrlQueue.GetDropCount()
	     + HatHotReqQueue.GetDropCount()
	     + LosSegReqQueue.GetDropCount()
	     + LosVectReqQueue.GetDropCount()
	     + WaveStateQueue.GetDropCount()
	     + ConfClampQueue.GetDropCount()
	     + MaritimeSurfaceQueue.GetDropCount();
}
