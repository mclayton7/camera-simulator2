// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UCamSimSubsystem;
class ACesiumGeoreference;
class FCigiSender;

/**
 * FCigiQueryHandler
 *
 * Game-thread object that drains HAT/HOT and LOS query queues each tick,
 * executes UE line traces to answer them, and stages results via FCigiSender
 * for transmission in the same frame's SOF datagram.
 *
 * Owned by UCamSimSubsystem; Tick() must be called from the game thread.
 */
class FCigiQueryHandler
{
public:
	FCigiQueryHandler(UCamSimSubsystem* InSubsystem, FCigiSender* InSender);

	/** Drain all pending query queues and stage responses. */
	void Tick(float DeltaTime);

private:
	void ProcessHatHotRequests(UWorld* World, ACesiumGeoreference* GeoRef);
	void ProcessLosSegRequests(UWorld* World, ACesiumGeoreference* GeoRef);
	void ProcessLosVectRequests(UWorld* World, ACesiumGeoreference* GeoRef);

	/** Geodetic (Lon, Lat, AltM) → UE world position (cm). */
	FVector GeoToWorld(ACesiumGeoreference* GeoRef, double Lat, double Lon, double AltM) const;

	/** Convert UE hit world position back to geodetic, return altitude in metres. */
	double WorldToAlt(ACesiumGeoreference* GeoRef, const FVector& WorldPos) const;

	/** If HitActor is an ACamSimEntity, return its EntityId; otherwise 0. */
	uint16 ResolveEntityId(const AActor* HitActor) const;

	UCamSimSubsystem* Subsystem = nullptr;
	FCigiSender*      Sender    = nullptr;
};
