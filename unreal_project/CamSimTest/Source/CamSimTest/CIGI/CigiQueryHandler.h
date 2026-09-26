// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UCamSimSubsystem;
class FCigiSender;
class FCamSimGeospatialProvider;

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
	void ProcessHatHotRequests(UWorld* World, const FCamSimGeospatialProvider& GeoProvider);
	void ProcessLosSegRequests(UWorld* World, const FCamSimGeospatialProvider& GeoProvider);
	void ProcessLosVectRequests(UWorld* World, const FCamSimGeospatialProvider& GeoProvider);

	/** Geodetic (Lat, Lon, AltM) → UE world position (cm). */
	bool GeoToWorld(UWorld* World, const FCamSimGeospatialProvider& GeoProvider,
		double Lat, double Lon, double AltM, FVector& OutWorld) const;

	/** Convert UE world position back to geodetic. */
	bool WorldToGeo(UWorld* World, const FCamSimGeospatialProvider& GeoProvider,
		const FVector& WorldPos, double& OutLat, double& OutLon, double& OutAltM) const;

	/**
	 * Resolve a CIGI point to geodetic. When bEntityRelative, (A, B, C) are
	 * X/Y/Z offsets in the body frame of EntityId; otherwise (lat, lon, alt).
	 * Returns false if the reference entity doesn't exist.
	 */
	bool ResolvePoint(bool bEntityRelative, uint16 EntityId, double A, double B, double C,
		double& OutLat, double& OutLon, double& OutAlt) const;

	/** If HitActor is an ACamSimEntity, return its EntityId; otherwise 0. */
	uint16 ResolveEntityId(const AActor* HitActor) const;

	UCamSimSubsystem* Subsystem = nullptr;
	FCigiSender*      Sender    = nullptr;
};
