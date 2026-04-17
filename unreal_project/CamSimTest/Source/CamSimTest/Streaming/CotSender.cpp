// Copyright CamSim Contributors. All Rights Reserved.

#include "Streaming/CotSender.h"
#include "Config/CamSimConfig.h"
#include "CamSimTest.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Common/UdpSocketBuilder.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Misc/DateTime.h"

// -------------------------------------------------------------------------
// Constructor / Destructor
// -------------------------------------------------------------------------

FCotSender::FCotSender(const FCamSimConfig& InConfig)
	: Config(InConfig)
{
}

FCotSender::~FCotSender()
{
	Close();
}

// -------------------------------------------------------------------------
// Open — create UDP socket
// -------------------------------------------------------------------------

bool FCotSender::Open()
{
	if (bOpen) return true;

	Socket = FUdpSocketBuilder(TEXT("CotSocket"))
		.AsNonBlocking()
		.AsReusable()
		.WithSendBufferSize(65536)
		.Build();

	if (!Socket)
	{
		UE_LOG(LogCamSim, Error, TEXT("FCotSender: failed to create UDP socket"));
		return false;
	}

	// Set multicast TTL if address is multicast (239.x.x.x)
	FIPv4Address DestIp;
	if (FIPv4Address::Parse(Config.Streaming.CotAddr, DestIp))
	{
		if (DestIp.IsMulticastAddress())
		{
			Socket->SetMulticastTtl(4);
		}
	}

	DestAddr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
	bool bValid = false;
	DestAddr->SetIp(*Config.Streaming.CotAddr, bValid);
	DestAddr->SetPort(Config.Streaming.CotPort);

	if (!bValid)
	{
		UE_LOG(LogCamSim, Error, TEXT("FCotSender: invalid CoT address '%s'"),
			*Config.Streaming.CotAddr);
		Close();
		return false;
	}

	bOpen = true;
	AccumulatedSec = 0.0f;
	UE_LOG(LogCamSim, Log, TEXT("FCotSender: opened (addr=%s:%d interval=%.1fs uid=%s)"),
		*Config.Streaming.CotAddr, Config.Streaming.CotPort,
		Config.Streaming.CotIntervalSec, *Config.Streaming.CotUid);
	return true;
}

// -------------------------------------------------------------------------
// Close
// -------------------------------------------------------------------------

void FCotSender::Close()
{
	if (!bOpen) return;
	bOpen = false;

	if (Socket)
	{
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}
	DestAddr.Reset();
	UE_LOG(LogCamSim, Log, TEXT("FCotSender: closed"));
}

// -------------------------------------------------------------------------
// Tick — accumulate delta, send when interval elapses
// -------------------------------------------------------------------------

void FCotSender::Tick(float DeltaTime, const FCamSimTelemetry& Telemetry)
{
	if (!bOpen) return;

	AccumulatedSec += DeltaTime;
	if (AccumulatedSec < Config.Streaming.CotIntervalSec) return;

	AccumulatedSec -= Config.Streaming.CotIntervalSec;

	const FString Xml = BuildCotXml(Telemetry);
	if (!Xml.IsEmpty()) SendCotPacket(Xml);
}

// -------------------------------------------------------------------------
// BuildCotXml — format CoT event XML
// -------------------------------------------------------------------------

FString FCotSender::BuildCotXml(const FCamSimTelemetry& Telemetry) const
{
	// Reject frames with non-finite or out-of-bounds geodetic before we emit
	// XML that would poison downstream ATAK/TAK databases. Return empty so
	// the caller skips Send(); a real telemetry frame will follow at the next
	// CotIntervalSec interval.
	if (!FMath::IsFinite(Telemetry.Latitude) || !FMath::IsFinite(Telemetry.Longitude)
	    || !FMath::IsFinite(Telemetry.Altitude) || !FMath::IsFinite(Telemetry.Yaw))
	{
		UE_LOG(LogCamSim, Warning,
			TEXT("FCotSender: non-finite telemetry (lat=%f lon=%f alt=%f yaw=%f) — skipping frame"),
			Telemetry.Latitude, Telemetry.Longitude, Telemetry.Altitude, Telemetry.Yaw);
		return FString();
	}
	if (FMath::Abs(Telemetry.Latitude) > 90.0 || FMath::Abs(Telemetry.Longitude) > 180.0)
	{
		UE_LOG(LogCamSim, Warning,
			TEXT("FCotSender: out-of-bounds geodetic (%f, %f) — skipping frame"),
			Telemetry.Latitude, Telemetry.Longitude);
		return FString();
	}

	const FDateTime Now = FDateTime::UtcNow();
	const FDateTime Stale = Now + FTimespan::FromSeconds(
		static_cast<double>(Config.Streaming.CotIntervalSec) * 2.0);

	const FString TimeStr  = Now.ToIso8601();
	const FString StaleStr = Stale.ToIso8601();

	return FString::Printf(
		TEXT("<?xml version='1.0' encoding='UTF-8'?>"
		     "<event version=\"2.0\" uid=\"%s\" type=\"%s\" "
		     "time=\"%s\" start=\"%s\" stale=\"%s\" how=\"m-g\">"
		     "<point lat=\"%.6f\" lon=\"%.6f\" hae=\"%.1f\" ce=\"9999999\" le=\"9999999\"/>"
		     "<detail>"
		     "<track course=\"%.1f\" speed=\"0.0\"/>"
		     "<contact callsign=\"%s\"/>"
		     "<__group name=\"Cyan\" role=\"ISR\"/>"
		     "</detail>"
		     "</event>"),
		*Config.Streaming.CotUid,
		*Config.Streaming.CotType,
		*TimeStr, *TimeStr, *StaleStr,
		Telemetry.Latitude, Telemetry.Longitude, Telemetry.Altitude,
		Telemetry.Yaw,
		*Config.Streaming.CotCallsign);
}

// -------------------------------------------------------------------------
// SendCotPacket — transmit XML over UDP
// -------------------------------------------------------------------------

void FCotSender::SendCotPacket(const FString& XmlStr)
{
	if (!Socket || !DestAddr.IsValid()) return;

	auto Utf8 = StringCast<ANSICHAR>(*XmlStr);
	const uint8* Data = reinterpret_cast<const uint8*>(Utf8.Get());
	const int32 Len = FCStringAnsi::Strlen(Utf8.Get());

	int32 BytesSent = 0;
	Socket->SendTo(Data, Len, BytesSent, *DestAddr);
}
