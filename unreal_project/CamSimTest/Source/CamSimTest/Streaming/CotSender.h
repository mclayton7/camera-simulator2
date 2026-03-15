// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Metadata/KlvBuilder.h"  // FCamSimTelemetry

struct FCamSimConfig;

/**
 * FCotSender
 *
 * Sends Cursor-on-Target (CoT) XML position reports over UDP multicast.
 * Game-thread class — CoT rate is 0.5-5 Hz, too slow to warrant a thread.
 * Follows FCigiSender pattern: FUdpSocketBuilder, accumulate delta, send on interval.
 */
class FCotSender
{
public:
	explicit FCotSender(const FCamSimConfig& Config);
	~FCotSender();

	bool Open();
	void Close();
	void Tick(float DeltaTime, const FCamSimTelemetry& Telemetry);

	/** Public for testing. */
	FString BuildCotXml(const FCamSimTelemetry& Telemetry) const;

private:
	const FCamSimConfig& Config;
	FSocket* Socket = nullptr;
	TSharedPtr<FInternetAddr> DestAddr;
	float AccumulatedSec = 0.0f;
	bool bOpen = false;

	void SendCotPacket(const FString& XmlStr);
};
