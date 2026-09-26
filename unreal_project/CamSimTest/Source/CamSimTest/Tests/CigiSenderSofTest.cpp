// Copyright CamSim Contributors. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Common/UdpSocketBuilder.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "CIGI/CigiSender.h"
#include "Config/CamSimConfig.h"

// -------------------------------------------------------------------------
// SOF (opcode 101) must echo the host's full 32-bit IG Control frame counter.
// Previously it was truncated to 8 bits, wrapping every 256 host frames.
// -------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCigiSenderSofHostFrameTest,
	"CamSim.CigiSender.SofEchoesFull32BitHostFrame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCigiSenderSofHostFrameTest::RunTest(const FString& Parameters)
{
	ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!TestNotNull(TEXT("Socket subsystem"), SS)) return false;

	// Listener on an ephemeral loopback port
	FSocket* Listener = FUdpSocketBuilder(TEXT("SofTestListener"))
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
	const bool bOpened = Sender.Open(Config);
	TestTrue(TEXT("Sender opened"), bOpened);

	constexpr uint32 HostFrame = 0x12345678u;  // > 255, distinct bytes
	if (bOpened)
	{
		Sender.FlushFrame(/*FrameCntr=*/7, HostFrame, /*IGMode=*/1);
	}

	TArray<uint8> Buf;
	Buf.SetNumZeroed(1500);
	int32 BytesRead = 0;
	const double Deadline = FPlatformTime::Seconds() + 2.0;
	while (FPlatformTime::Seconds() < Deadline)
	{
		if (Listener->RecvFrom(Buf.GetData(), Buf.Num(), BytesRead, *SS->CreateInternetAddr()) && BytesRead > 0)
		{
			break;
		}
		FPlatformProcess::Sleep(0.01f);
	}

	Sender.Close();
	SS->DestroySocket(Listener);

	// CIGI 3.3 SOF: opcode 101, size 24; Last Host Frame Counter at bytes 16-19.
	if (!TestTrue(TEXT("Received SOF datagram"), BytesRead >= 24 && Buf[0] == 101 && Buf[1] == 24))
	{
		return false;
	}
	const uint32 BigEndian    = (uint32(Buf[16]) << 24) | (uint32(Buf[17]) << 16) | (uint32(Buf[18]) << 8) | Buf[19];
	const uint32 LittleEndian = (uint32(Buf[19]) << 24) | (uint32(Buf[18]) << 16) | (uint32(Buf[17]) << 8) | Buf[16];
	TestTrue(FString::Printf(TEXT("Last host frame echoed in full (BE 0x%08X / LE 0x%08X)"), BigEndian, LittleEndian),
		BigEndian == HostFrame || LittleEndian == HostFrame);
	return true;
}
