// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Common/UdpSocketBuilder.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "CIGI/CigiReceiver.h"
#include "CIGI/CigiPacketTypes.h"
#include "Config/CamSimConfig.h"
#include "Geospatial/CigiFrames.h"

THIRD_PARTY_INCLUDES_START
#include "cigicl/CigiHostSession.h"
#include "cigicl/CigiOutgoingMsg.h"
#include "cigicl/CigiIGCtrlV3_3.h"
#include "cigicl/CigiEntityCtrlV3_3.h"
#include "cigicl/CigiHatHotReqV3_2.h"
#include "cigicl/CigiLosSegReqV3_2.h"
#include "cigicl/CigiLosVectReqV3_2.h"
#include "cigicl/CigiRateCtrlV3_2.h"
THIRD_PARTY_INCLUDES_END

// -------------------------------------------------------------------------
// Entity-relative CIGI coordinates: offsets in metres must never be read as
// latitude/longitude degrees.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCigiEntityRelativeMathTest,
	"CamSim.CigiEntityRelative.Math",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCigiEntityRelativeMathTest::RunTest(const FString& Parameters)
{
	using namespace CamSimFrames;

	// Parent flying east (heading 90) at 45N, 1000 m.
	FGeoPose Parent;
	Parent.Lat = 45.0; Parent.Lon = -100.0; Parent.Alt = 1000.0;
	Parent.Neu = CigiToNeu(90.0, 0.0, 0.0);

	// Child 100 m ahead, 10 m right, 5 m below; yawed 90 right of the parent.
	const FGeoPose Child = AttachedChildPose(Parent, FVector(100.0, 10.0, 5.0), 90.0, 0.0, 0.0);
	const double MetresPerDegLon = 78846.8;  // WGS-84 at 45N
	const double MetresPerDegLat = 111131.7;
	TestTrue(FString::Printf(TEXT("Ahead = east: dLon %.2f m"), (Child.Lon - Parent.Lon) * MetresPerDegLon),
		FMath::IsNearlyEqual((Child.Lon - Parent.Lon) * MetresPerDegLon, 100.0, 0.5));
	TestTrue(FString::Printf(TEXT("Right = south: dLat %.2f m"), (Child.Lat - Parent.Lat) * MetresPerDegLat),
		FMath::IsNearlyEqual((Child.Lat - Parent.Lat) * MetresPerDegLat, -10.0, 0.5));
	TestTrue(TEXT("Below = lower altitude"), FMath::IsNearlyEqual(Child.Alt, 995.0, 1e-6));
	TestTrue(TEXT("Child heading = parent + 90 = south"),
		FMath::IsNearlyEqual(FRotator::ClampAxis(Child.Neu.Rotator().Yaw), 180.0, 1e-3));

	// Entity-relative LOS vector straight ahead and 10 deg up from a nose-down
	// (pitch -10) parent heading east: level, towards the east.
	FGeoPose Diving = Parent;
	Diving.Neu = CigiToNeu(90.0, -10.0, 0.0);
	double Az = 0.0, El = 0.0;
	BodyAzElToTrueAzEl(Diving.Neu, 0.0, 10.0, Az, El);
	TestTrue(FString::Printf(TEXT("Vector az %.3f ~= 90"), Az), FMath::IsNearlyEqual(Az, 90.0, 1e-3));
	TestTrue(FString::Printf(TEXT("Vector el %.3f ~= 0"), El), FMath::IsNearlyEqual(El, 0.0, 1e-3));

	// Body azimuth 90 = the entity's right side.
	BodyAzElToTrueAzEl(Parent.Neu, 90.0, 0.0, Az, El);
	TestTrue(FString::Printf(TEXT("Right-side vector points south (az %.3f)"), Az), FMath::IsNearlyEqual(Az, 180.0, 1e-3));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCigiEntityRelativeParseTest,
	"CamSim.CigiEntityRelative.ReceiverCarriesFlags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCigiEntityRelativeParseTest::RunTest(const FString& Parameters)
{
	FCamSimConfig Config;
	Config.CigiBindAddr   = TEXT("127.0.0.1");
	Config.CigiPort       = 48871;
	Config.CameraEntityId = 0;

	FCigiReceiver Receiver(Config);
	if (!TestTrue(TEXT("Receiver started"), Receiver.Start()))
	{
		return false;
	}

	// Build one host datagram with CCL, exactly as a CIGI 3.3 host would.
	CigiHostSession Session(1, 4096, 2, 4096);
	Session.SetCigiVersion(3, 3);
	CigiOutgoingMsg& Out = Session.GetOutgoingMsgMgr();

	CigiIGCtrlV3_3 IgCtrl;
	IgCtrl.SetFrameCntr(1000);
	IgCtrl.SetTimeStampValid(true);
	IgCtrl.SetTimeStamp(500000);  // 5.0 s in 10 us ticks

	CigiEntityCtrlV3_3 Child;
	Child.SetEntityID(5);
	Child.SetEntityState(CigiBaseEntityCtrl::Active);
	Child.SetAttachState(CigiBaseEntityCtrl::Attach);
	Child.SetParentID(1);
	Child.SetXoff(12.5);
	Child.SetYoff(-3.0);
	Child.SetZoff(2.0);
	Child.SetYaw(30.0f);

	CigiHatHotReqV3_2 HatHot;
	HatHot.SetHatHotID(7);
	HatHot.SetReqType(CigiBaseHatHotReq::HAT);
	HatHot.SetSrcCoordSys(CigiBaseHatHotReq::Entity);
	HatHot.SetEntityID(1);
	HatHot.SetXoff(50.0);
	HatHot.SetYoff(0.0);
	HatHot.SetZoff(0.0);

	CigiLosSegReqV3_2 LosSeg;
	LosSeg.SetLosID(8);
	LosSeg.SetSrcCoordSys(CigiBaseLosSegReq::Entity);
	LosSeg.SetDstCoordSys(CigiBaseLosSegReq::Entity);
	LosSeg.SetEntityID(1);
	LosSeg.SetDestEntityIDValid(true);
	LosSeg.SetDestEntityID(9);
	LosSeg.SetSrcXoff(1.0);
	LosSeg.SetDstXoff(2.0);

	CigiLosVectReqV3_2 LosVect;
	LosVect.SetLosID(10);
	LosVect.SetSrcCoordSys(CigiBaseLosVectReq::Entity);
	LosVect.SetEntityID(1);
	LosVect.SetVectAz(15.0f);
	LosVect.SetVectEl(-20.0f);
	LosVect.SetMaxRange(5000.0f);

	CigiRateCtrlV3_2 Rate;
	Rate.SetEntityID(5);
	Rate.SetCoordSys(CigiBaseRateCtrl::World);
	Rate.SetXRate(40.0f);

	Out.BeginMsg();
	Out << IgCtrl;
	Out << Child;
	Out << Rate;
	Out << HatHot;
	Out << LosSeg;
	Out << LosVect;
	Cigi_uint8* Buf = nullptr;
	int Len = 0;
	const bool bPackaged = (Out.PackageMsg(&Buf, Len) == CIGI_SUCCESS) && Buf && Len > 0;
	TestTrue(TEXT("Host message packaged"), bPackaged);

	ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	FSocket* Tx = FUdpSocketBuilder(TEXT("CigiTestHost")).Build();
	TSharedRef<FInternetAddr> Dest = SS->CreateInternetAddr();
	bool bValidIp = false;
	Dest->SetIp(TEXT("127.0.0.1"), bValidIp);
	Dest->SetPort(Config.CigiPort);
	int32 Sent = 0;
	if (bPackaged && Tx)
	{
		Tx->SendTo(Buf, Len, Sent, *Dest);
		Out.FreeMsg();
	}

	FCigiEntityState State;
	FCigiHatHotRequest HatReq;
	FCigiLosSegRequest SegReq;
	FCigiLosVectRequest VectReq;
	FCigiRateControl RateCtrl;
	bool bState = false, bHat = false, bSeg = false, bVect = false, bRate = false;
	const double Deadline = FPlatformTime::Seconds() + 2.0;
	while (FPlatformTime::Seconds() < Deadline && !(bState && bHat && bSeg && bVect && bRate))
	{
		bRate  |= Receiver.DequeueRateControl(RateCtrl);
		bState |= Receiver.DequeueEntityState(State);
		bHat   |= Receiver.DequeueHatHotRequest(HatReq);
		bSeg   |= Receiver.DequeueLosSegRequest(SegReq);
		bVect  |= Receiver.DequeueLosVectRequest(VectReq);
		FPlatformProcess::Sleep(0.005f);
	}
	Receiver.Stop();
	if (Tx) SS->DestroySocket(Tx);

	if (TestTrue(TEXT("Entity Control received"), bState))
	{
		TestEqual(TEXT("Entity id"), State.EntityId, static_cast<uint16>(5));
		TestTrue(TEXT("Attach state carried"), State.bAttached);
		TestEqual(TEXT("Parent id"), State.ParentId, static_cast<uint16>(1));
		TestTrue(TEXT("Offsets kept as metres"), FMath::IsNearlyEqual(State.Latitude, 12.5)
			&& FMath::IsNearlyEqual(State.Longitude, -3.0) && FMath::IsNearlyEqual(State.Altitude, 2.0f));
		TestTrue(TEXT("Host timestamp used (5.0 s)"), FMath::IsNearlyEqual(State.HostTimeSec, 5.0, 1e-6));
	}
	if (TestTrue(TEXT("Rate Control received"), bRate))
	{
		TestEqual(TEXT("Rate entity id"), RateCtrl.EntityId, static_cast<uint16>(5));
		TestFalse(TEXT("World/Parent coordinate system carried"), RateCtrl.bLocalFrame);
		TestTrue(TEXT("X rate"), FMath::IsNearlyEqual(RateCtrl.XRate, 40.0f));
	}
	if (TestTrue(TEXT("HAT/HOT request received"), bHat))
	{
		TestTrue(TEXT("HAT/HOT entity-relative"), HatReq.bEntityRelative);
		TestTrue(TEXT("HAT/HOT X offset"), FMath::IsNearlyEqual(HatReq.Lat, 50.0));
	}
	if (TestTrue(TEXT("LOS segment request received"), bSeg))
	{
		TestTrue(TEXT("LOS seg source entity-relative"), SegReq.bSrcEntityRelative);
		TestTrue(TEXT("LOS seg destination entity-relative"), SegReq.bDstEntityRelative);
		TestTrue(TEXT("Destination entity id valid"), SegReq.bDestEntityIDValid);
		TestEqual(TEXT("Destination entity id"), SegReq.DestEntityId, static_cast<uint16>(9));
	}
	if (TestTrue(TEXT("LOS vector request received"), bVect))
	{
		TestTrue(TEXT("LOS vector entity-relative"), VectReq.bEntityRelative);
		TestTrue(TEXT("LOS vector azimuth kept body-relative"), FMath::IsNearlyEqual(VectReq.VectAz, 15.0f));
	}
	return true;
}
