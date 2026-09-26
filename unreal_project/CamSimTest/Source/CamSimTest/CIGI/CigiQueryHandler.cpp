// Copyright CamSim Contributors. All Rights Reserved.

#include "CIGI/CigiQueryHandler.h"
#include "CIGI/CigiSender.h"
#include "CIGI/CigiPacketTypes.h"
#include "CIGI/CigiReceiver.h"
#include "Subsystem/CamSimSubsystem.h"
#include "Entity/CamSimEntity.h"
#include "Geospatial/CamSimGeospatialProvider.h"
#include "CamSimTest.h"

#include "Engine/World.h"

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

	const FCamSimGeospatialProvider* GeoProvider = Subsystem->GetGeospatialProvider();
	if (!GeoProvider || !GeoProvider->IsAvailable(World)) return;

	ProcessHatHotRequests(World, *GeoProvider);
	ProcessLosSegRequests(World, *GeoProvider);
	ProcessLosVectRequests(World, *GeoProvider);
}

// -------------------------------------------------------------------------
// HAT/HOT (opcode 24)
// -------------------------------------------------------------------------

void FCigiQueryHandler::ProcessHatHotRequests(UWorld* World, const FCamSimGeospatialProvider& GeoProvider)
{
	FCigiReceiver* Receiver = Subsystem->GetCigiReceiver();
	if (!Receiver) return;

	FCigiHatHotRequest Req;
	while (Receiver->DequeueHatHotRequest(Req))
	{
		const bool bExtended = (Req.ReqType == 2);
		auto RespondInvalid = [&]()
		{
			if (bExtended) Sender->EnqueueHatHotExtendedResponse(Req.HatHotId, false, 0.0, 0.0, 0, 0.0f, 0.0f);
			else           Sender->EnqueueHatHotResponse(Req.HatHotId, false, Req.ReqType, 0.0, 0.0);
		};
		if (Req.ReqType > 2)
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("FCigiQueryHandler: unsupported HAT/HOT ReqType=%u (id=%u) -> invalid response"),
				static_cast<uint32>(Req.ReqType), static_cast<uint32>(Req.HatHotId));
			RespondInvalid();
			continue;
		}

		if (!ResolvePoint(Req.bEntityRelative, Req.EntityId, Req.Lat, Req.Lon, Req.Alt, Req.Lat, Req.Lon, Req.Alt))
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("FCigiQueryHandler: HAT/HOT id=%u relative to unknown entity %u"),
				static_cast<uint32>(Req.HatHotId), static_cast<uint32>(Req.EntityId));
			RespondInvalid();
			continue;
		}

		// Trace from high above the query point straight down to below sea level.
		// The query altitude is the reference point; we start the trace above any
		// possible terrain regardless of query alt.
		FVector TopPt = FVector::ZeroVector;
		FVector BotPt = FVector::ZeroVector;
		if (!GeoToWorld(World, GeoProvider, Req.Lat, Req.Lon, HATHOT_TRACE_TOP_M, TopPt) ||
		    !GeoToWorld(World, GeoProvider, Req.Lat, Req.Lon, HATHOT_TRACE_BOTTOM_M, BotPt))
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("FCigiQueryHandler: failed geo->world for HAT/HOT id=%u lat=%.6f lon=%.6f"),
				static_cast<uint32>(Req.HatHotId), Req.Lat, Req.Lon);
			RespondInvalid();
			continue;
		}

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
			double HitLat = 0.0;
			double HitLon = 0.0;
			if (!WorldToGeo(World, GeoProvider, HitResult.Location, HitLat, HitLon, HOT))
			{
				UE_LOG(LogCamSim, Warning, TEXT("FCigiQueryHandler: failed world->geo transform for HAT/HOT id=%u"),
					static_cast<uint32>(Req.HatHotId));
				RespondInvalid();
				continue;
			}
			HAT    = Req.Alt - HOT;  // height above terrain
			bValid = true;
		}

		if (bExtended)
		{
			// CamSim has no terrain material data yet: material code 0.
			float NormalAz = 0.0f, NormalEl = 90.0f;
			if (bValid)
			{
				SurfaceNormalAzEl(World, GeoProvider, HitResult, NormalAz, NormalEl);
			}
			Sender->EnqueueHatHotExtendedResponse(Req.HatHotId, bValid, HAT, HOT, 0, NormalAz, NormalEl);
		}
		else
		{
			Sender->EnqueueHatHotResponse(Req.HatHotId, bValid, Req.ReqType, HAT, HOT);
		}
	}
}

// -------------------------------------------------------------------------
// LOS Segment (opcode 25)
// -------------------------------------------------------------------------

void FCigiQueryHandler::ProcessLosSegRequests(UWorld* World, const FCamSimGeospatialProvider& GeoProvider)
{
	FCigiReceiver* Receiver = Subsystem->GetCigiReceiver();
	if (!Receiver) return;

	FCigiLosSegRequest Req;
	while (Receiver->DequeueLosSegRequest(Req))
	{
		const uint16 DstRefId = Req.bDestEntityIDValid ? Req.DestEntityId : Req.EntityId;
		if (!ResolvePoint(Req.bSrcEntityRelative, Req.EntityId, Req.SrcLat, Req.SrcLon, Req.SrcAlt,
		                  Req.SrcLat, Req.SrcLon, Req.SrcAlt) ||
		    !ResolvePoint(Req.bDstEntityRelative, DstRefId, Req.DstLat, Req.DstLon, Req.DstAlt,
		                  Req.DstLat, Req.DstLon, Req.DstAlt))
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("FCigiQueryHandler: LOS seg id=%u relative to unknown entity"),
				static_cast<uint32>(Req.LosId));
			RespondLosInvalid(Req.LosId, Req.ReqType);
			continue;
		}

		FVector SrcWorld = FVector::ZeroVector;
		FVector DstWorld = FVector::ZeroVector;
		if (!GeoToWorld(World, GeoProvider, Req.SrcLat, Req.SrcLon, Req.SrcAlt, SrcWorld) ||
		    !GeoToWorld(World, GeoProvider, Req.DstLat, Req.DstLon, Req.DstAlt, DstWorld))
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("FCigiQueryHandler: failed geo->world for LOS seg id=%u"),
				static_cast<uint32>(Req.LosId));
			RespondLosInvalid(Req.LosId, Req.ReqType);
			continue;
		}

		FHitResult HitResult;
		FCollisionQueryParams QueryParams;
		QueryParams.bReturnPhysicalMaterial = false;

		const bool bHit = World->LineTraceSingleByChannel(
			HitResult, SrcWorld, DstWorld, ECC_Visibility, QueryParams);

		RespondLos(World, GeoProvider, Req.LosId, Req.ReqType, /*bSegment=*/true, Req.bResponseEntityCs,
			bHit ? &HitResult : nullptr, SrcWorld, DstWorld, Req.DstLat, Req.DstLon, Req.DstAlt);
	}
}

// -------------------------------------------------------------------------
// LOS Vector (opcode 26)
// -------------------------------------------------------------------------

void FCigiQueryHandler::ProcessLosVectRequests(UWorld* World, const FCamSimGeospatialProvider& GeoProvider)
{
	FCigiReceiver* Receiver = Subsystem->GetCigiReceiver();
	if (!Receiver) return;

	FCigiLosVectRequest Req;
	while (Receiver->DequeueLosVectRequest(Req))
	{
		if (Req.bEntityRelative)
		{
			// Source offset and vector are both in the entity's body frame.
			CamSimFrames::FGeoPose Ref;
			if (!Subsystem->GetEntityGeoPose(FEntityKey(EHostSource::Cigi, Req.EntityId), Ref))
			{
				UE_LOG(LogCamSim, Warning,
					TEXT("FCigiQueryHandler: LOS vect id=%u relative to unknown entity %u"),
					static_cast<uint32>(Req.LosId), static_cast<uint32>(Req.EntityId));
				RespondLosInvalid(Req.LosId, Req.ReqType);
				continue;
			}
			CamSimFrames::BodyOffsetToGeodetic(Ref, FVector(Req.SrcLat, Req.SrcLon, Req.SrcAlt),
				Req.SrcLat, Req.SrcLon, Req.SrcAlt);
			double TrueAz = 0.0, TrueEl = 0.0;
			CamSimFrames::BodyAzElToTrueAzEl(Ref.Neu, Req.VectAz, Req.VectEl, TrueAz, TrueEl);
			Req.VectAz = static_cast<float>(TrueAz);
			Req.VectEl = static_cast<float>(TrueEl);
		}

		// End point of the vector: a local North/East/Up displacement from the source.
		const double AzRad   = FMath::DegreesToRadians(static_cast<double>(Req.VectAz));
		const double ElRad   = FMath::DegreesToRadians(static_cast<double>(Req.VectEl));
		const double MaxRngM = static_cast<double>(Req.MaxRange);

		// Horizontal range component and up component
		const double HorizM = MaxRngM * FMath::Cos(ElRad);
		const double UpM    = MaxRngM * FMath::Sin(ElRad);

		// North/East displacements (positive North = Az=0, positive East = Az=90)
		const double NorthM = HorizM * FMath::Cos(AzRad);
		const double EastM  = HorizM * FMath::Sin(AzRad);

		double EndLat, EndLon, EndAlt;
		CamSimFrames::OffsetGeodetic(Req.SrcLat, Req.SrcLon, Req.SrcAlt, FVector(NorthM, EastM, UpM),
			EndLat, EndLon, EndAlt);

		FVector SrcWorld = FVector::ZeroVector;
		FVector EndWorld = FVector::ZeroVector;
		if (!GeoToWorld(World, GeoProvider, Req.SrcLat, Req.SrcLon, Req.SrcAlt, SrcWorld) ||
		    !GeoToWorld(World, GeoProvider, EndLat, EndLon, EndAlt, EndWorld))
		{
			UE_LOG(LogCamSim, Warning,
				TEXT("FCigiQueryHandler: failed geo->world for LOS vect id=%u"),
				static_cast<uint32>(Req.LosId));
			RespondLosInvalid(Req.LosId, Req.ReqType);
			continue;
		}

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

		// Range is measured from the source point, not the min-range start.
		RespondLos(World, GeoProvider, Req.LosId, Req.ReqType, /*bSegment=*/false, Req.bResponseEntityCs,
			bHit ? &HitResult : nullptr, SrcWorld, EndWorld, EndLat, EndLon, EndAlt);
	}
}

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

bool FCigiQueryHandler::GeoToWorld(
	UWorld* World, const FCamSimGeospatialProvider& GeoProvider,
	double Lat, double Lon, double AltM, FVector& OutWorld) const
{
	if (!GeoProvider.GeoToWorld(World, Lat, Lon, AltM, OutWorld))
	{
		UE_LOG(LogCamSim, Warning, TEXT("FCigiQueryHandler: geospatial GeoToWorld failed (lat=%.6f lon=%.6f alt=%.2f)"),
			Lat, Lon, AltM);
		return false;
	}
	return true;
}

bool FCigiQueryHandler::WorldToGeo(
	UWorld* World, const FCamSimGeospatialProvider& GeoProvider,
	const FVector& WorldPos, double& OutLat, double& OutLon, double& OutAltM) const
{
	return GeoProvider.WorldToGeo(World, WorldPos, OutLat, OutLon, OutAltM);
}

bool FCigiQueryHandler::ResolvePoint(bool bEntityRelative, uint16 EntityId, double A, double B, double C,
	double& OutLat, double& OutLon, double& OutAlt) const
{
	if (!bEntityRelative)
	{
		OutLat = A; OutLon = B; OutAlt = C;
		return true;
	}
	CamSimFrames::FGeoPose Ref;
	if (!Subsystem->GetEntityGeoPose(FEntityKey(EHostSource::Cigi, EntityId), Ref)) return false;
	CamSimFrames::BodyOffsetToGeodetic(Ref, FVector(A, B, C), OutLat, OutLon, OutAlt);
	return true;
}

uint16 FCigiQueryHandler::ResolveEntityId(const AActor* HitActor) const
{
	// Only the CIGI host's own entities: another source's ID would mean
	// nothing to it (DIS and scenario entities are "not an entity").
	const ACamSimEntity* Entity = Cast<ACamSimEntity>(HitActor);
	if (Entity && Entity->Key.Source == EHostSource::Cigi && Entity->Key.Id <= MAX_uint16)
	{
		return static_cast<uint16>(Entity->Key.Id);
	}
	return 0;
}

bool FCigiQueryHandler::SurfaceNormalAzEl(UWorld* World, const FCamSimGeospatialProvider& GeoProvider,
	const FHitResult& Hit, float& OutAzDeg, float& OutElDeg) const
{
	// Geodetic positions of the hit and of a point 1 m along the normal give
	// the normal in the local North-East-Up frame at the hit.
	double Lat0, Lon0, Alt0, Lat1, Lon1, Alt1;
	if (!WorldToGeo(World, GeoProvider, Hit.ImpactPoint, Lat0, Lon0, Alt0) ||
	    !WorldToGeo(World, GeoProvider, Hit.ImpactPoint + Hit.ImpactNormal * UE_CM_PER_METRE, Lat1, Lon1, Alt1))
	{
		return false;
	}
	double Az, El;
	CamSimFrames::NeuToAzEl(CamSimFrames::GeodeticDeltaToNeu(Lat0, Lon0, Alt0, Lat1, Lon1, Alt1), Az, El);
	OutAzDeg = static_cast<float>(Az);
	OutElDeg = static_cast<float>(El);
	return true;
}

void FCigiQueryHandler::RespondLosInvalid(uint16 LosId, uint8 ReqType)
{
	if (ReqType == 1)
	{
		FCigiLosExtendedResponse Resp;
		Resp.LosId = LosId;
		Sender->EnqueueLosExtendedResponse(Resp);
	}
	else
	{
		Sender->EnqueueLosResponse(LosId, false, false, 0.0, 0.0, 0.0, 0.0, 0, false);
	}
}

void FCigiQueryHandler::RespondLos(UWorld* World, const FCamSimGeospatialProvider& GeoProvider,
	uint16 LosId, uint8 ReqType, bool bSegment, bool bResponseEntityCs,
	const FHitResult* Hit, const FVector& SrcWorld, const FVector& DstWorld,
	double DstLat, double DstLon, double DstAlt)
{
	// Intersection point (or, for a clear segment, the destination) in geodetic.
	double Lat = DstLat, Lon = DstLon, Alt = DstAlt;
	double Range = FVector::Dist(SrcWorld, DstWorld) / UE_CM_PER_METRE;
	uint16 EntityId = 0;
	if (Hit)
	{
		if (!WorldToGeo(World, GeoProvider, Hit->Location, Lat, Lon, Alt))
		{
			UE_LOG(LogCamSim, Warning, TEXT("FCigiQueryHandler: failed world->geo transform for LOS id=%u"),
				static_cast<uint32>(LosId));
			RespondLosInvalid(LosId, ReqType);
			return;
		}
		Range    = FVector::Dist(SrcWorld, Hit->Location) / UE_CM_PER_METRE;
		EntityId = ResolveEntityId(Hit->GetActor());
	}

	if (ReqType != 1)
	{
		// Basic response (ICD 4.2.4): Valid means the Range is valid, i.e. an
		// intersection was found; Visible = the segment is unobstructed.
		Sender->EnqueueLosResponse(LosId, Hit != nullptr, bSegment && Hit == nullptr,
			Range, Lat, Lon, Alt, EntityId, EntityId != 0);
		return;
	}

	// Extended response (ICD 4.2.5). A segment always reports a point: the
	// occluding surface, or the destination when it is visible; its Range is
	// flagged invalid. A vector reports only an intersection.
	FCigiLosExtendedResponse Resp;
	Resp.LosId          = LosId;
	Resp.bValid         = bSegment || Hit != nullptr;
	Resp.bRangeValid    = !bSegment && Hit != nullptr;
	Resp.bVisible       = bSegment && Hit == nullptr;
	Resp.EntityId       = EntityId;
	Resp.bEntityIdValid = EntityId != 0;
	Resp.Range          = Range;
	Resp.LatOrX = Lat;
	Resp.LonOrY = Lon;
	Resp.AltOrZ = Alt;

	CamSimFrames::FGeoPose EntityPose;
	if (bResponseEntityCs && Resp.bEntityIdValid && Subsystem->GetEntityGeoPose(FEntityKey(EHostSource::Cigi, EntityId), EntityPose))
	{
		const FVector Offset = CamSimFrames::GeodeticToBodyOffset(EntityPose, Lat, Lon, Alt);
		Resp.bEntityCs = true;
		Resp.LatOrX = Offset.X;
		Resp.LonOrY = Offset.Y;
		Resp.AltOrZ = Offset.Z;
	}
	if (Hit)
	{
		SurfaceNormalAzEl(World, GeoProvider, *Hit, Resp.NormalAzDeg, Resp.NormalElDeg);
	}
	Sender->EnqueueLosExtendedResponse(Resp);
}
