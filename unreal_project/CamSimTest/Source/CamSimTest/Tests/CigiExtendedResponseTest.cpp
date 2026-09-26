// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Common/UdpSocketBuilder.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "CIGI/CigiSender.h"
#include "CIGI/CigiPacketTypes.h"
#include "Config/CamSimConfig.h"

THIRD_PARTY_INCLUDES_START
#include "cigicl/CigiHostSession.h"
#include "cigicl/CigiIncomingMsg.h"
#include "cigicl/CigiBaseEventProcessor.h"
#include "cigicl/CigiHatHotXRespV3_2.h"
#include "cigicl/CigiLosXRespV3_2.h"
THIRD_PARTY_INCLUDES_END

// -------------------------------------------------------------------------
// HAT/HOT Extended Response (opcode 103) and LOS Extended Response (105)
// decode on a CIGI 3.3 host exactly as staged.
// -------------------------------------------------------------------------

namespace
{
	struct FHatHotX { uint16 Id; bool bValid; double Hat, Hot; float Az, El; };
	struct FLosX    { uint16 Id; bool bValid, bRangeValid, bVisible, bEntityValid; uint16 Entity;
	                  double Range, X, Y, Z; float Az, El; };

	class FHatHotXCollector : public CigiBaseEventProcessor
	{
	public:
		TArray<FHatHotX> Got;
		void OnPacketReceived(CigiBasePacket* Packet) override
		{
			const auto* P = static_cast<CigiHatHotXRespV3_2*>(Packet);
			Got.Add({ P->GetHatHotID(), P->GetValid(), P->GetHat(), P->GetHot(), P->GetNormAz(), P->GetNormEl() });
		}
	};

	class FLosXCollector : public CigiBaseEventProcessor
	{
	public:
		TArray<FLosX> Got;
		void OnPacketReceived(CigiBasePacket* Packet) override
		{
			const auto* P = static_cast<CigiLosXRespV3_2*>(Packet);
			Got.Add({ P->GetLosID(), P->GetValid(), P->GetRangeValid(), P->GetVisible(), P->GetEntityIDValid(),
				P->GetEntityID(), P->GetRange(), P->GetXoff(), P->GetYoff(), P->GetZoff(),
				P->GetNormalAz(), P->GetNormalEl() });
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCigiExtendedResponseTest,
	"CamSim.CigiSender.ExtendedResponsesDecode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCigiExtendedResponseTest::RunTest(const FString& Parameters)
{
	ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	FSocket* Listener = FUdpSocketBuilder(TEXT("XRespTestListener"))
		.AsNonBlocking()
		.BoundToAddress(FIPv4Address(127, 0, 0, 1))
		.BoundToPort(0)
		.Build();
	if (!TestNotNull(TEXT("Listener socket"), Listener)) return false;
	TSharedRef<FInternetAddr> ListenAddr = SS->CreateInternetAddr();
	Listener->GetAddress(*ListenAddr);

	FCamSimConfig Config;
	Config.CigiResponseAddr = TEXT("127.0.0.1");
	Config.CigiResponsePort = ListenAddr->GetPort();

	FCigiSender Sender;
	if (!TestTrue(TEXT("Sender opened"), Sender.Open(Config)))
	{
		SS->DestroySocket(Listener);
		return false;
	}

	Sender.EnqueueHatHotExtendedResponse(11, true, 123.5, 45.25, 0, -30.0f, 60.0f);
	FCigiLosExtendedResponse Hit;
	Hit.LosId = 12; Hit.bValid = true; Hit.bRangeValid = true;
	Hit.bEntityIdValid = true; Hit.EntityId = 7; Hit.bEntityCs = true;
	Hit.Range = 1500.0; Hit.LatOrX = 12.5; Hit.LonOrY = -250.0; Hit.AltOrZ = 3.0;  // offsets beyond ±90/±180
	Hit.NormalAzDeg = 10.0f; Hit.NormalElDeg = 80.0f;
	Sender.EnqueueLosExtendedResponse(Hit);
	FCigiLosExtendedResponse Clear;
	Clear.LosId = 13; Clear.bValid = true; Clear.bVisible = true;
	Clear.Range = 800.0; Clear.LatOrX = 45.5; Clear.LonOrY = -122.25; Clear.AltOrZ = 100.0;
	Sender.EnqueueLosExtendedResponse(Clear);
	Sender.FlushFrame(1, 0, 1);

	TArray<uint8> Buf;
	Buf.SetNumZeroed(4096);
	int32 BytesRead = 0;
	const double Deadline = FPlatformTime::Seconds() + 2.0;
	while (FPlatformTime::Seconds() < Deadline
		&& !(Listener->RecvFrom(Buf.GetData(), Buf.Num(), BytesRead, *SS->CreateInternetAddr()) && BytesRead > 0))
	{
		FPlatformProcess::Sleep(0.01f);
	}
	Sender.Close();
	SS->DestroySocket(Listener);
	if (!TestTrue(TEXT("Received IG datagram"), BytesRead > 0)) return false;

	CigiHostSession Host(1, 4096, 2, 4096);
	Host.SetCigiVersion(3, 3);
	CigiIncomingMsg& In = Host.GetIncomingMsgMgr();
	In.SetReaderCigiVersion(3, 3);
	FHatHotXCollector HatHot;
	FLosXCollector Los;
	In.RegisterEventProcessor(CIGI_HAT_HOT_XRESP_PACKET_ID_V3_2, &HatHot);
	In.RegisterEventProcessor(CIGI_LOS_XRESP_PACKET_ID_V3_2, &Los);
	In.ProcessIncomingMsg(Buf.GetData(), BytesRead);

	if (TestEqual(TEXT("one HAT/HOT extended response"), HatHot.Got.Num(), 1))
	{
		const FHatHotX& R = HatHot.Got[0];
		TestEqual(TEXT("HAT/HOT id"), R.Id, static_cast<uint16>(11));
		TestTrue(TEXT("HAT/HOT valid"), R.bValid);
		TestEqual(TEXT("HAT"), R.Hat, 123.5);
		TestEqual(TEXT("HOT"), R.Hot, 45.25);
		TestEqual(TEXT("normal azimuth"), R.Az, -30.0f);
		TestEqual(TEXT("normal elevation"), R.El, 60.0f);
	}
	if (TestEqual(TEXT("two LOS extended responses"), Los.Got.Num(), 2))
	{
		const FLosX& A = Los.Got[0];
		TestEqual(TEXT("LOS id"), A.Id, static_cast<uint16>(12));
		TestTrue(TEXT("valid + range valid"), A.bValid && A.bRangeValid);
		TestTrue(TEXT("entity id"), A.bEntityValid && A.Entity == 7);
		TestEqual(TEXT("range"), A.Range, 1500.0);
		TestTrue(TEXT("entity-frame offsets survive (no lat/lon clamping)"),
			A.X == 12.5 && A.Y == -250.0 && A.Z == 3.0);
		TestEqual(TEXT("normal elevation"), A.El, 80.0f);

		const FLosX& B = Los.Got[1];
		TestTrue(TEXT("clear segment: valid, visible, range invalid"), B.bValid && B.bVisible && !B.bRangeValid);
		TestTrue(TEXT("clear segment reports the destination"), B.X == 45.5 && B.Y == -122.25 && B.Z == 100.0);
	}
	return true;
}
