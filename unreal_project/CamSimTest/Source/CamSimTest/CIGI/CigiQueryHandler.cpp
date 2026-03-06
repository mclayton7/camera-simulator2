// Copyright CamSim Contributors. All Rights Reserved.

#include "CIGI/CigiQueryHandler.h"
#include "CIGI/CigiSender.h"
#include "CIGI/CigiPacketTypes.h"
#include "CIGI/CigiReceiver.h"
#include "Subsystem/CamSimSubsystem.h"
#include "Entity/CamSimEntity.h"
#include "CamSimTest.h"

#include "Engine/World.h"
#include "CesiumGeoreference.h"

// 1 metre in UE units (UE default: 1 unit = 1 cm → 100 units/metre)
static constexpr double UE_CM_PER_METRE = 100.0;

// Trace ceiling — start HAT/HOT traces well above any possible terrain
static constexpr double HATHOT_TRACE_TOP_M    = 50000.0;
// Trace floor — allow for below-sea-level terrain
static constexpr double HATHOT_TRACE_BOTTOM_M = -500.0;

// -------------------------------------------------------------------------
// Constructor
// -------------------------------------------------------------------------

FCigiQueryHandler::FCigiQueryHandler(UCamSimSubsystem* InSubsystem, FCigiSender* InSender)
	: Subsystem(InSubsystem)
	, Sender(InSender)
{
}

// -------------------------------------------------------------------------
// Tick
// -------------------------------------------------------------------------

void FCigiQueryHandler::Tick(float DeltaTime)
{
	if (!Subsystem || !Sender) return;

	UWorld* World = Subsystem->GetGameInstance()->GetWorld();
	if (!World) return;

	ACesiumGeoreference* GeoRef =
		ACesiumGeoreference::GetDefaultGeoreference(World);
	if (!GeoRef) return;

	ProcessHatHotRequests(World, GeoRef);
	ProcessLosSegRequests(World, GeoRef);
	ProcessLosVectRequests(World, GeoRef);
}

// -------------------------------------------------------------------------
// HAT/HOT (opcode 24)
// -------------------------------------------------------------------------

void FCigiQueryHandler::ProcessHatHotRequests(UWorld* World, ACesiumGeoreference* GeoRef)
{
	FCigiReceiver* Receiver = Subsystem->GetCigiReceiver();
	if (!Receiver) return;

	FCigiHatHotRequest Req;
	while (Receiver->DequeueHatHotRequest(Req))
	{
		if (Req.ReqType > 2)
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("FCigiQueryHandler: unsupported HAT/HOT ReqType=%u (id=%u) -> invalid response"),
				static_cast<uint32>(Req.ReqType), static_cast<uint32>(Req.HatHotId));
			Sender->EnqueueHatHotResponse(Req.HatHotId, false, 0, 0.0, 0.0);
			continue;
		}

		if (Req.ReqType == 2)
		{
			// CIGI v3.3 response packet does not provide extended fields.
			// We downgrade to basic semantics and return HAT/HOT values.
			UE_LOG(LogCamSim, Verbose,
				TEXT("FCigiQueryHandler: HAT/HOT ReqType=2 (extended) downgraded to basic response (id=%u)"),
				static_cast<uint32>(Req.HatHotId));
		}

		// Trace from high above the query point straight down to below sea level.
		// The query altitude is the reference point; we start the trace above any
		// possible terrain regardless of query alt.
		const FVector TopPt = GeoToWorld(GeoRef, Req.Lat, Req.Lon, HATHOT_TRACE_TOP_M);
		const FVector BotPt = GeoToWorld(GeoRef, Req.Lat, Req.Lon, HATHOT_TRACE_BOTTOM_M);

		FHitResult HitResult;
		FCollisionQueryParams QueryParams;
		QueryParams.bReturnPhysicalMaterial = false;

		const bool bHit = World->LineTraceSingleByChannel(
			HitResult, TopPt, BotPt, ECC_Visibility, QueryParams);

		double HAT = 0.0;
		double HOT = 0.0;
		bool bValid = false;

		if (bHit)
		{
			// HOT = terrain altitude above WGS-84 ellipsoid at the hit point
			HOT    = WorldToAlt(GeoRef, HitResult.Location);
			HAT    = Req.Alt - HOT;  // height above terrain
			bValid = true;
		}

		Sender->EnqueueHatHotResponse(Req.HatHotId, bValid, Req.ReqType, HAT, HOT);
	}
}

// -------------------------------------------------------------------------
// LOS Segment (opcode 25)
// -------------------------------------------------------------------------

void FCigiQueryHandler::ProcessLosSegRequests(UWorld* World, ACesiumGeoreference* GeoRef)
{
	FCigiReceiver* Receiver = Subsystem->GetCigiReceiver();
	if (!Receiver) return;

	FCigiLosSegRequest Req;
	while (Receiver->DequeueLosSegRequest(Req))
	{
		const FVector SrcWorld = GeoToWorld(GeoRef, Req.SrcLat, Req.SrcLon, Req.SrcAlt);
		const FVector DstWorld = GeoToWorld(GeoRef, Req.DstLat, Req.DstLon, Req.DstAlt);

		FHitResult HitResult;
		FCollisionQueryParams QueryParams;
		QueryParams.bReturnPhysicalMaterial = false;

		const bool bHit = World->LineTraceSingleByChannel(
			HitResult, SrcWorld, DstWorld, ECC_Visibility, QueryParams);

		bool   bValid       = bHit;
		bool   bVisible     = !bHit;   // visible = no obstruction
		double Range        = 0.0;
		double HitLat       = 0.0;
		double HitLon       = 0.0;
		double HitAlt       = 0.0;
		uint16 EntityId     = 0;
		bool   bEntityValid = false;

		if (bHit)
		{
			// Range in metres (Dist returns UE units = cm)
			Range = static_cast<double>(FVector::Dist(SrcWorld, HitResult.Location))
			        / UE_CM_PER_METRE;

			// Convert hit back to geodetic
			const FVector LLH = GeoRef->TransformUnrealPositionToLongitudeLatitudeHeight(
				HitResult.Location);
			HitLon = LLH.X;
			HitLat = LLH.Y;
			HitAlt = LLH.Z;

			EntityId     = ResolveEntityId(HitResult.GetActor());
			bEntityValid = (EntityId != 0);
		}

		Sender->EnqueueLosResponse(Req.LosId, bValid, bVisible,
			Range, HitLat, HitLon, HitAlt, EntityId, bEntityValid);
	}
}

// -------------------------------------------------------------------------
// LOS Vector (opcode 26)
// -------------------------------------------------------------------------

void FCigiQueryHandler::ProcessLosVectRequests(UWorld* World, ACesiumGeoreference* GeoRef)
{
	FCigiReceiver* Receiver = Subsystem->GetCigiReceiver();
	if (!Receiver) return;

	FCigiLosVectRequest Req;
	while (Receiver->DequeueLosVectRequest(Req))
	{
		// Compute the end point of the vector in geodetic using a flat-earth
		// approximation at the source location (consistent with ComputeGeometricLOS).
		const double AzRad   = FMath::DegreesToRadians(static_cast<double>(Req.VectAz));
		const double ElRad   = FMath::DegreesToRadians(static_cast<double>(Req.VectEl));
		const double MaxRngM = static_cast<double>(Req.MaxRange);

		// Horizontal range component and up component
		const double HorizM = MaxRngM * FMath::Cos(ElRad);
		const double UpM    = MaxRngM * FMath::Sin(ElRad);

		// North/East displacements (positive North = Az=0, positive East = Az=90)
		const double NorthM = HorizM * FMath::Cos(AzRad);
		const double EastM  = HorizM * FMath::Sin(AzRad);

		// Flat-earth conversion to geodetic delta
		const double LatRad   = FMath::DegreesToRadians(Req.SrcLat);
		const double DeltaLat = NorthM / 111320.0;
		const double DeltaLon = EastM  / (111320.0 * FMath::Cos(LatRad));

		const double EndLat = Req.SrcLat + DeltaLat;
		const double EndLon = Req.SrcLon + DeltaLon;
		const double EndAlt = Req.SrcAlt + UpM;

		const FVector SrcWorld = GeoToWorld(GeoRef, Req.SrcLat, Req.SrcLon, Req.SrcAlt);
		const FVector EndWorld = GeoToWorld(GeoRef, EndLat, EndLon, EndAlt);

		// Skip minimum-range portion by starting trace at min-range offset if needed
		FVector TraceStart = SrcWorld;
		if (Req.MinRange > 0.0f)
		{
			const double MinFrac = static_cast<double>(Req.MinRange) / MaxRngM;
			TraceStart = FMath::Lerp(SrcWorld, EndWorld, MinFrac);
		}

		FHitResult HitResult;
		FCollisionQueryParams QueryParams;
		QueryParams.bReturnPhysicalMaterial = false;

		const bool bHit = World->LineTraceSingleByChannel(
			HitResult, TraceStart, EndWorld, ECC_Visibility, QueryParams);

		bool   bValid       = bHit;
		bool   bVisible     = !bHit;
		double Range        = 0.0;
		double HitLat       = 0.0;
		double HitLon       = 0.0;
		double HitAlt       = 0.0;
		uint16 EntityId     = 0;
		bool   bEntityValid = false;

		if (bHit)
		{
			// Range measured from original source point
			Range = static_cast<double>(FVector::Dist(SrcWorld, HitResult.Location))
			        / UE_CM_PER_METRE;

			const FVector LLH = GeoRef->TransformUnrealPositionToLongitudeLatitudeHeight(
				HitResult.Location);
			HitLon = LLH.X;
			HitLat = LLH.Y;
			HitAlt = LLH.Z;

			EntityId     = ResolveEntityId(HitResult.GetActor());
			bEntityValid = (EntityId != 0);
		}

		Sender->EnqueueLosResponse(Req.LosId, bValid, bVisible,
			Range, HitLat, HitLon, HitAlt, EntityId, bEntityValid);
	}
}

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

FVector FCigiQueryHandler::GeoToWorld(ACesiumGeoreference* GeoRef,
                                      double Lat, double Lon, double AltM) const
{
	// Cesium expects (Longitude, Latitude, Height) in that order
	return GeoRef->TransformLongitudeLatitudeHeightPositionToUnreal(
		FVector(Lon, Lat, AltM));
}

double FCigiQueryHandler::WorldToAlt(ACesiumGeoreference* GeoRef,
                                     const FVector& WorldPos) const
{
	// TransformUnrealPositionToLongitudeLatitudeHeight returns (Lon, Lat, AltM)
	const FVector LLH = GeoRef->TransformUnrealPositionToLongitudeLatitudeHeight(WorldPos);
	return LLH.Z;  // altitude in metres
}

uint16 FCigiQueryHandler::ResolveEntityId(const AActor* HitActor) const
{
	if (const ACamSimEntity* Entity = Cast<ACamSimEntity>(HitActor))
	{
		return Entity->EntityId;
	}
	return 0;
}
