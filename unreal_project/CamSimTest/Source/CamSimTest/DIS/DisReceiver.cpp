// Copyright CamSim Contributors. All Rights Reserved.

#include "DIS/DisReceiver.h"
#include "Config/CamSimConfig.h"
#include "CamSimTest.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Common/UdpSocketBuilder.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "HAL/PlatformFileManager.h"

// -------------------------------------------------------------------------
// Constructor / Destructor
// -------------------------------------------------------------------------

FDisReceiver::FDisReceiver(const FCamSimConfig& InConfig)
	: Config(InConfig)
	, bShouldRun(false)
{
}

FDisReceiver::~FDisReceiver()
{
	Stop();
}

// -------------------------------------------------------------------------
// Start / Stop
// -------------------------------------------------------------------------

bool FDisReceiver::Start()
{
	if (!CreateSocket())
	{
		UE_LOG(LogCamSim, Error, TEXT("FDisReceiver: failed to create UDP socket on port %d"),
			Config.DIS.Port);
		return false;
	}

	bShouldRun = true;
	if (!ShutdownEvent)
	{
		ShutdownEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/false);
	}
	Thread.Reset(FRunnableThread::Create(this, TEXT("DisReceiverThread"), 128 * 1024,
		TPri_Normal, FPlatformAffinity::GetTaskGraphBackgroundTaskMask()));

	UE_LOG(LogCamSim, Log, TEXT("FDisReceiver: listening on %s:%d (exercise=%d timeout=%.1fs)"),
		*Config.DIS.BindAddr, Config.DIS.Port,
		Config.DIS.ExerciseId, Config.DIS.HeartbeatTimeoutSec);
	return Thread.IsValid();
}

void FDisReceiver::Stop()
{
	bShouldRun = false;
	if (ShutdownEvent) ShutdownEvent->Trigger();
	if (Thread.IsValid())
	{
		Thread->WaitForCompletion();
		Thread.Reset();
	}
	if (Socket)
	{
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}
	if (ShutdownEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(ShutdownEvent);
		ShutdownEvent = nullptr;
	}
	UE_LOG(LogCamSim, Log, TEXT("FDisReceiver: stopped (parsed %llu PDUs)"),
		ReceivedPduCount.Load());
}

// -------------------------------------------------------------------------
// FRunnable interface
// -------------------------------------------------------------------------

bool FDisReceiver::Init()
{
	return true;
}

uint32 FDisReceiver::Run()
{
	static constexpr int32 RecvBufSize = 32768;
	uint8 RecvBuf[RecvBufSize];

	const uint8 FilterExerciseId = static_cast<uint8>(Config.DIS.ExerciseId);

	while (bShouldRun)
	{
		int32 BytesRead = 0;
		if (Socket && Socket->Recv(RecvBuf, RecvBufSize, BytesRead) && BytesRead > 0)
		{
			++ReceivedPacketCount;

			// Parse PDU header for filtering
			FDisPduHeader Hdr;
			if (!FDisPduHeader::Parse(RecvBuf, BytesRead, Hdr))
			{
				continue;
			}

			// Filter by exercise ID (0 = accept all)
			if (FilterExerciseId != 0 && Hdr.ExerciseId != FilterExerciseId)
			{
				continue;
			}

			if (Hdr.PduType == EDisPduType::EntityState)
			{
				FDisEntityStatePdu Pdu;
				if (FDisEntityStatePdu::Parse(RecvBuf, BytesRead, Pdu))
				{
					EntityStatePduQueue.Enqueue(MoveTemp(Pdu));
					++ReceivedPduCount;
				}
			}
			else if (Hdr.PduType == EDisPduType::Designator)
			{
				FDisDesignatorPdu DesigPdu;
				if (FDisDesignatorPdu::Parse(RecvBuf, BytesRead, DesigPdu))
				{
					DesignatorPduQueue.Enqueue(MoveTemp(DesigPdu));
					++ReceivedPduCount;
				}
			}
		}
		else
		{
			// Non-blocking socket returned no data — wait on the shutdown event
			// so we exit cleanly the instant Stop() fires.
			if (ShutdownEvent) ShutdownEvent->Wait(1);
			else FPlatformProcess::SleepNoStats(0.001f);
		}
	}

	return 0;
}

void FDisReceiver::Exit()
{
	// No CCL session to tear down — all parsing is inline
}

// -------------------------------------------------------------------------
// Socket creation (with multicast join)
// -------------------------------------------------------------------------

bool FDisReceiver::CreateSocket()
{
	FIPv4Address BindAddress;
	if (!FIPv4Address::Parse(Config.DIS.BindAddr, BindAddress))
	{
		BindAddress = FIPv4Address::Any;
	}

	Socket = FUdpSocketBuilder(TEXT("DisSocket"))
		.AsNonBlocking()
		.AsReusable()
		.BoundToAddress(BindAddress)
		.BoundToPort(Config.DIS.Port)
		.WithReceiveBufferSize(256 * 1024)
		.Build();

	if (!Socket) return false;

	// Join multicast group if configured
	if (!Config.DIS.MulticastGroup.IsEmpty())
	{
		FIPv4Address MulticastAddr;
		if (FIPv4Address::Parse(Config.DIS.MulticastGroup, MulticastAddr))
		{
			const TSharedRef<FInternetAddr> GroupAddr =
				ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
			GroupAddr->SetIp(MulticastAddr.Value);

			if (Socket->JoinMulticastGroup(*GroupAddr))
			{
				UE_LOG(LogCamSim, Log, TEXT("FDisReceiver: joined multicast group %s"),
					*Config.DIS.MulticastGroup);
			}
			else
			{
				// Don't silently carry on — an operator who configured a
				// multicast group expects multicast data. Returning false
				// surfaces the failure at Start() instead of looking like a
				// healthy but oddly-empty unicast receiver.
				UE_LOG(LogCamSim, Error,
					TEXT("FDisReceiver: failed to join multicast group %s (port %d)"),
					*Config.DIS.MulticastGroup, Config.DIS.Port);
				ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
				Socket = nullptr;
				return false;
			}
		}
	}

	return true;
}
