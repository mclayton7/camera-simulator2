// Copyright CamSim Contributors. All Rights Reserved.

#include "Scenario/ScenarioEngine.h"
#include "Entity/CamSimEntity.h"
#include "CamSimTest.h"
#include "CesiumGlobeAnchorComponent.h"

static constexpr uint8  CIGI_ENTITY_ACTIVE = 1;
#include "Geospatial/GeoConstants.h"
static constexpr double DEG_TO_RAD = PI / 180.0;

// ---------------------------------------------------------------------------
// Geo helpers
// ---------------------------------------------------------------------------

double FScenarioEngine::DistanceM(double Lat1, double Lon1, double Lat2, double Lon2)
{
	const double dLat = (Lat2 - Lat1) * DEG_TO_RAD;
	const double dLon = (Lon2 - Lon1) * DEG_TO_RAD;
	const double a = FMath::Sin(dLat * 0.5) * FMath::Sin(dLat * 0.5) +
	                 FMath::Cos(Lat1 * DEG_TO_RAD) * FMath::Cos(Lat2 * DEG_TO_RAD) *
	                 FMath::Sin(dLon * 0.5) * FMath::Sin(dLon * 0.5);
	const double c = 2.0 * FMath::Atan2(FMath::Sqrt(a), FMath::Sqrt(1.0 - a));
	return 6371000.0 * c; // Earth radius in metres
}

float FScenarioEngine::BearingDeg(double Lat1, double Lon1, double Lat2, double Lon2)
{
	const double dLon = (Lon2 - Lon1) * DEG_TO_RAD;
	const double lat1R = Lat1 * DEG_TO_RAD;
	const double lat2R = Lat2 * DEG_TO_RAD;
	const double y = FMath::Sin(dLon) * FMath::Cos(lat2R);
	const double x = FMath::Cos(lat1R) * FMath::Sin(lat2R) -
	                 FMath::Sin(lat1R) * FMath::Cos(lat2R) * FMath::Cos(dLon);
	float Bearing = static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(y, x)));
	if (Bearing < 0.0f) Bearing += 360.0f;
	return Bearing;
}

// ---------------------------------------------------------------------------
// Initialize / Reset
// ---------------------------------------------------------------------------

void FScenarioEngine::Initialize(const FCamSimConfig& Config)
{
	EntityConfigs = Config.ScenarioEntities;
	Triggers      = Config.ScenarioTriggers;
	TimeScale     = Config.ScenarioTimeScale;
	StartHour     = Config.ScenarioStartHour;
	FrameCount    = 0;

	TriggerStates.SetNum(Triggers.Num());
	for (FTriggerState& TS : TriggerStates)
	{
		TS.bFired = false;
		TS.LastFireTimeSec = 0.0;
	}

	WaypointStates.Empty();
	SpeedOverrides.Empty();
	PendingRemovals.Empty();
	LastKnownStates.Empty();

	SortByFormationDependency();
}

void FScenarioEngine::Reset()
{
	WaypointStates.Empty();
	TriggerStates.Empty();
	SpeedOverrides.Empty();
	PendingRemovals.Empty();
	LastKnownStates.Empty();
	FrameCount = 0;
}

void FScenarioEngine::SetEntitySpeed(uint16 EntityId, float NewSpeedMps)
{
	SpeedOverrides.Add(EntityId, NewSpeedMps);
}

TArray<uint16> FScenarioEngine::GetPendingRemovals() const
{
	return PendingRemovals;
}

// ---------------------------------------------------------------------------
// Tick — main entry point
// ---------------------------------------------------------------------------

TArray<FCigiEntityState> FScenarioEngine::Tick(
    double ScenarioElapsedSec, float DeltaTimeSec,
    float TimeOfDayHours,
    const TMap<uint16, ACamSimEntity*>& EntityMap)
{
	PendingRemovals.Reset();
	++FrameCount;

	TArray<FCigiEntityState> Output;
	Output.Reserve(EntityConfigs.Num());

	for (const FCamSimConfig::FScenarioEntityConfig& Spec : EntityConfigs)
	{
		const uint16 EId = static_cast<uint16>(FMath::Clamp(Spec.EntityId, 0, 65535));

		// Not yet spawned
		if (ScenarioElapsedSec < static_cast<double>(Spec.SpawnTimeSec))
			continue;

		// 23C: Activity schedule — check if this entity is active at current time-of-day
		if (Spec.ActivitySchedule.Num() > 0)
		{
			const int32 ActiveIdx = ResolveActiveSchedule(Spec, TimeOfDayHours);
			if (ActiveIdx < 0) continue; // not in any active schedule window
		}

		FCigiEntityState State;
		if (Spec.Waypoints.Num() >= 2)
		{
			FWaypointState& WpState = WaypointStates.FindOrAdd(EId);
			State = InterpolateWaypoints(Spec, WpState, ScenarioElapsedSec);
		}
		else
		{
			State = BuildLinearState(Spec, ScenarioElapsedSec);
		}

		Output.Add(State);
		LastKnownStates.Add(State.EntityId, State);
	}

	// 23D: Apply formation offsets (followers derived from leader positions)
	ApplyFormationOffsets(Output);

	// 23B: Evaluate triggers after entity states are computed
	EvaluateTriggers(ScenarioElapsedSec, EntityMap);

	return Output;
}

// ---------------------------------------------------------------------------
// 23A: Waypoint interpolation
// ---------------------------------------------------------------------------

FCigiEntityState FScenarioEngine::InterpolateWaypoints(
    const FCamSimConfig::FScenarioEntityConfig& Spec,
    FWaypointState& State, double ScenarioElapsedSec) const
{
	const TArray<FCamSimConfig::FWaypointConfig>& Wps = Spec.Waypoints;
	const int32 NumWps = Wps.Num();
	const uint16 EId = static_cast<uint16>(FMath::Clamp(Spec.EntityId, 0, 65535));

	// Initialize segment start time on first call
	if (State.SegmentStartTime <= 0.0 && !State.bFinished)
	{
		State.SegmentStartTime = FMath::Max(ScenarioElapsedSec, static_cast<double>(Spec.SpawnTimeSec));
	}

	// If finished (non-looping), hold at last waypoint
	if (State.bFinished)
	{
		const FCamSimConfig::FWaypointConfig& Last = Wps.Last();
		FCigiEntityState Out;
		Out.EntityId    = EId;
		Out.EntityType  = static_cast<uint16>(FMath::Clamp(Spec.EntityType, 0, 65535));
		Out.EntityState = CIGI_ENTITY_ACTIVE;
		Out.Latitude    = Last.Latitude;
		Out.Longitude   = Last.Longitude;
		Out.Altitude    = Last.Altitude;
		Out.Yaw         = (State.CurrentIdx > 0)
			? BearingDeg(Wps[State.CurrentIdx - 1].Latitude, Wps[State.CurrentIdx - 1].Longitude,
			             Last.Latitude, Last.Longitude)
			: Spec.StartYaw;
		return Out;
	}

	// Handle pause at current waypoint
	if (State.bPaused)
	{
		State.PauseRemaining -= (1.0f / 30.0f); // fixed frame time
		if (State.PauseRemaining <= 0.0f)
		{
			State.bPaused = false;
			// Advance to next segment
			State.CurrentIdx++;
			if (State.CurrentIdx >= NumWps - 1)
			{
				if (Spec.bLoopWaypoints)
				{
					State.CurrentIdx = 0;
				}
				else
				{
					State.bFinished = true;
				}
			}
			State.SegmentStartTime = ScenarioElapsedSec;
		}
		else
		{
			// Hold at current waypoint
			const FCamSimConfig::FWaypointConfig& Cur = Wps[FMath::Min(State.CurrentIdx + 1, NumWps - 1)];
			FCigiEntityState Out;
			Out.EntityId    = EId;
			Out.EntityType  = static_cast<uint16>(FMath::Clamp(Spec.EntityType, 0, 65535));
			Out.EntityState = CIGI_ENTITY_ACTIVE;
			Out.Latitude    = Cur.Latitude;
			Out.Longitude   = Cur.Longitude;
			Out.Altitude    = Cur.Altitude;
			Out.Yaw         = (State.CurrentIdx < NumWps - 1)
				? BearingDeg(Cur.Latitude, Cur.Longitude,
				             Wps[State.CurrentIdx + 1].Latitude, Wps[State.CurrentIdx + 1].Longitude)
				: Spec.StartYaw;
			return Out;
		}
	}

	// Clamp current index
	const int32 CurIdx  = FMath::Clamp(State.CurrentIdx, 0, NumWps - 2);
	const int32 NextIdx = CurIdx + 1;
	const FCamSimConfig::FWaypointConfig& A = Wps[CurIdx];
	const FCamSimConfig::FWaypointConfig& B = Wps[NextIdx];

	// Determine speed for this segment
	float SegSpeed = A.SpeedMps > 0.0f ? A.SpeedMps : Spec.BaseSpeedMps;
	if (const float* Override = SpeedOverrides.Find(EId))
	{
		SegSpeed = *Override;
	}
	SegSpeed = FMath::Max(0.01f, SegSpeed);

	// Compute segment distance and transit time
	const double SegDist = DistanceM(A.Latitude, A.Longitude, B.Latitude, B.Longitude);
	const double TransitTime = SegDist / static_cast<double>(SegSpeed);

	// Compute progress alpha
	double Alpha = 0.0;
	if (TransitTime > 0.001)
	{
		Alpha = (ScenarioElapsedSec - State.SegmentStartTime) / TransitTime;
	}
	else
	{
		Alpha = 1.0; // zero-distance segment, skip immediately
	}
	Alpha = FMath::Clamp(Alpha, 0.0, 1.0);

	// Check if we've completed this segment
	if (Alpha >= 1.0)
	{
		// Check for pause at destination waypoint
		if (B.PauseSec > 0.0f && !State.bPaused)
		{
			State.bPaused = true;
			State.PauseRemaining = B.PauseSec;
			Alpha = 1.0;
		}
		else
		{
			// Advance to next segment
			State.CurrentIdx = NextIdx;
			if (State.CurrentIdx >= NumWps - 1)
			{
				if (Spec.bLoopWaypoints)
				{
					State.CurrentIdx = 0;
				}
				else
				{
					State.bFinished = true;
				}
			}
			State.SegmentStartTime = ScenarioElapsedSec;
			Alpha = 1.0;
		}
	}

	// Interpolate position
	const double InterpLat = A.Latitude  + (B.Latitude  - A.Latitude)  * Alpha;
	const double InterpLon = A.Longitude + (B.Longitude - A.Longitude) * Alpha;
	const float  InterpAlt = A.Altitude  + (B.Altitude  - A.Altitude)  * static_cast<float>(Alpha);

	// Auto heading
	float Heading;
	if (A.HeadingDeg >= 0.0f)
	{
		Heading = A.HeadingDeg;
	}
	else
	{
		Heading = BearingDeg(A.Latitude, A.Longitude, B.Latitude, B.Longitude);
	}

	FCigiEntityState Out;
	Out.EntityId    = EId;
	Out.EntityType  = static_cast<uint16>(FMath::Clamp(Spec.EntityType, 0, 65535));
	Out.EntityState = CIGI_ENTITY_ACTIVE;
	Out.Latitude    = InterpLat;
	Out.Longitude   = InterpLon;
	Out.Altitude    = InterpAlt;
	Out.Yaw         = Heading;
	Out.Pitch       = Spec.StartPitch;
	Out.Roll        = Spec.StartRoll;
	return Out;
}

// ---------------------------------------------------------------------------
// 23A: Linear rate fallback
// ---------------------------------------------------------------------------

FCigiEntityState FScenarioEngine::BuildLinearState(
    const FCamSimConfig::FScenarioEntityConfig& Spec,
    double ScenarioElapsedSec) const
{
	const double TimeSinceSpawn = FMath::Max(0.0, ScenarioElapsedSec - static_cast<double>(Spec.SpawnTimeSec));
	const double LatRad = FMath::DegreesToRadians(Spec.StartLatitude);
	const double CosLat = FMath::Max(0.01, FMath::Abs(FMath::Cos(LatRad)));

	FCigiEntityState Out;
	Out.EntityId    = static_cast<uint16>(FMath::Clamp(Spec.EntityId, 0, 65535));
	Out.EntityType  = static_cast<uint16>(FMath::Clamp(Spec.EntityType, 0, 65535));
	Out.EntityState = CIGI_ENTITY_ACTIVE;
	Out.Latitude    = Spec.StartLatitude  + (static_cast<double>(Spec.NorthRateMps) * TimeSinceSpawn) / METRES_PER_DEGREE_LAT;
	Out.Longitude   = Spec.StartLongitude + (static_cast<double>(Spec.EastRateMps) * TimeSinceSpawn)  / (METRES_PER_DEGREE_LAT * CosLat);
	Out.Altitude    = static_cast<float>(Spec.StartAltitude + static_cast<double>(Spec.UpRateMps) * TimeSinceSpawn);
	Out.Yaw         = Spec.StartYaw   + Spec.YawRateDegPerSec   * static_cast<float>(TimeSinceSpawn);
	Out.Pitch       = Spec.StartPitch + Spec.PitchRateDegPerSec * static_cast<float>(TimeSinceSpawn);
	Out.Roll        = Spec.StartRoll  + Spec.RollRateDegPerSec  * static_cast<float>(TimeSinceSpawn);
	return Out;
}

// ---------------------------------------------------------------------------
// 23B: Trigger evaluation
// ---------------------------------------------------------------------------

void FScenarioEngine::EvaluateTriggers(
    double ScenarioElapsedSec,
    const TMap<uint16, ACamSimEntity*>& EntityMap)
{
	for (int32 i = 0; i < Triggers.Num(); ++i)
	{
		const FCamSimConfig::FScenarioTrigger& Trig = Triggers[i];
		FTriggerState& TS = TriggerStates[i];

		// Skip if already fired and not repeating
		if (TS.bFired && !Trig.bRepeat) continue;

		// Check cooldown for repeating triggers
		if (TS.bFired && Trig.bRepeat && Trig.CooldownSec > 0.0f)
		{
			if ((ScenarioElapsedSec - TS.LastFireTimeSec) < static_cast<double>(Trig.CooldownSec))
				continue;
		}

		if (CheckCondition(Trig.Condition, ScenarioElapsedSec, EntityMap))
		{
			ExecuteAction(Trig.Action, ScenarioElapsedSec);
			TS.bFired = true;
			TS.LastFireTimeSec = ScenarioElapsedSec;
		}
	}
}

bool FScenarioEngine::CheckCondition(
    const FCamSimConfig::FScenarioCondition& C,
    double ScenarioElapsedSec,
    const TMap<uint16, ACamSimEntity*>& EntityMap) const
{
	switch (C.Type)
	{
	case FCamSimConfig::EScenarioConditionType::TimeSec:
		return ScenarioElapsedSec >= static_cast<double>(C.TimeSec);

	case FCamSimConfig::EScenarioConditionType::EntityInArea:
	{
		const uint16 EIdA = static_cast<uint16>(C.EntityIdA);
		ACamSimEntity* const* EntPtr = EntityMap.Find(EIdA);
		if (!EntPtr || !IsValid(*EntPtr)) return false;
		const UCesiumGlobeAnchorComponent* GA = (*EntPtr)->FindComponentByClass<UCesiumGlobeAnchorComponent>();
		if (!GA) return false;
		const FVector LLH = GA->GetLongitudeLatitudeHeight();
		const double Dist = DistanceM(LLH.Y, LLH.X, C.AreaLatitude, C.AreaLongitude);
		return Dist <= static_cast<double>(C.RadiusM);
	}

	case FCamSimConfig::EScenarioConditionType::EntityProximity:
	{
		const uint16 EIdA = static_cast<uint16>(C.EntityIdA);
		const uint16 EIdB = static_cast<uint16>(C.EntityIdB);
		ACamSimEntity* const* PtrA = EntityMap.Find(EIdA);
		ACamSimEntity* const* PtrB = EntityMap.Find(EIdB);
		if (!PtrA || !IsValid(*PtrA) || !PtrB || !IsValid(*PtrB)) return false;
		// Use UE world distance and convert cm -> m
		const double DistCm = FVector::Dist((*PtrA)->GetActorLocation(), (*PtrB)->GetActorLocation());
		return (DistCm / 100.0) <= static_cast<double>(C.RadiusM);
	}

	case FCamSimConfig::EScenarioConditionType::FrameCount:
		return FrameCount >= C.FrameThreshold;

	default:
		return false;
	}
}

void FScenarioEngine::ExecuteAction(
    const FCamSimConfig::FScenarioAction& A,
    double ScenarioElapsedSec)
{
	switch (A.Type)
	{
	case FCamSimConfig::EScenarioActionType::SpawnEntity:
	{
		// Produce an entity state that the entity manager will process
		FCamSimConfig::FScenarioEntityConfig SpawnSpec;
		SpawnSpec.EntityId       = A.TargetEntityId;
		SpawnSpec.EntityType     = A.TargetEntityType;
		SpawnSpec.StartLatitude  = A.SpawnLatitude;
		SpawnSpec.StartLongitude = A.SpawnLongitude;
		SpawnSpec.StartAltitude  = A.SpawnAltitude;
		SpawnSpec.SpawnTimeSec   = 0.0f;
		EntityConfigs.Add(SpawnSpec);
		UE_LOG(LogCamSim, Log, TEXT("ScenarioEngine: trigger spawning entity %d (type %d)"),
			A.TargetEntityId, A.TargetEntityType);
		break;
	}
	case FCamSimConfig::EScenarioActionType::DespawnEntity:
		PendingRemovals.Add(static_cast<uint16>(A.TargetEntityId));
		UE_LOG(LogCamSim, Log, TEXT("ScenarioEngine: trigger despawning entity %d"), A.TargetEntityId);
		break;

	case FCamSimConfig::EScenarioActionType::SetDamageState:
		UE_LOG(LogCamSim, Log, TEXT("ScenarioEngine: trigger set damage state %d on entity %d"),
			A.DamageState, A.TargetEntityId);
		break;

	case FCamSimConfig::EScenarioActionType::ChangeSpeed:
		SetEntitySpeed(static_cast<uint16>(A.TargetEntityId), A.NewSpeedMps);
		UE_LOG(LogCamSim, Log, TEXT("ScenarioEngine: trigger change speed to %.1f m/s on entity %d"),
			A.NewSpeedMps, A.TargetEntityId);
		break;

	case FCamSimConfig::EScenarioActionType::LogMessage:
		UE_LOG(LogCamSim, Log, TEXT("ScenarioEngine: trigger message: %s"), *A.Message);
		break;
	}
}

// ---------------------------------------------------------------------------
// 23C: Activity schedule resolution
// ---------------------------------------------------------------------------

int32 FScenarioEngine::ResolveActiveSchedule(
    const FCamSimConfig::FScenarioEntityConfig& Spec,
    float TimeOfDayHours) const
{
	for (const FCamSimConfig::FActivityScheduleEntry& Entry : Spec.ActivitySchedule)
	{
		if (TimeOfDayHours >= Entry.StartHour && TimeOfDayHours < Entry.EndHour)
		{
			return Entry.WaypointPathIndex;
		}
	}
	return -1; // not in any active window
}

// ---------------------------------------------------------------------------
// 23D: Formation flying
// ---------------------------------------------------------------------------

const FCamSimConfig::FScenarioEntityConfig* FScenarioEngine::FindSpecById(uint16 EId) const
{
	for (const FCamSimConfig::FScenarioEntityConfig& Spec : EntityConfigs)
	{
		if (static_cast<uint16>(FMath::Clamp(Spec.EntityId, 0, 65535)) == EId)
			return &Spec;
	}
	return nullptr;
}

void FScenarioEngine::ApplyFormationOffsets(TArray<FCigiEntityState>& Output)
{
	// Build index for O(1) leader lookup within current frame output
	TMap<uint16, int32> OutputIndexByID;
	for (int32 i = 0; i < Output.Num(); ++i)
	{
		OutputIndexByID.Add(Output[i].EntityId, i);
	}

	for (FCigiEntityState& Follower : Output)
	{
		const FCamSimConfig::FScenarioEntityConfig* Spec = FindSpecById(Follower.EntityId);
		if (!Spec || Spec->LeaderEntityId == 0)
			continue;

		const uint16 LeaderId = static_cast<uint16>(FMath::Clamp(Spec->LeaderEntityId, 0, 65535));

		// Find leader state: prefer current frame, fall back to last-known
		const FCigiEntityState* Leader = nullptr;
		if (const int32* Idx = OutputIndexByID.Find(LeaderId))
		{
			Leader = &Output[*Idx];
		}
		else if (const FCigiEntityState* Cached = LastKnownStates.Find(LeaderId))
		{
			Leader = Cached;
		}

		if (!Leader)
			continue; // leader never seen — skip

		// Rotate body-frame offset by leader heading
		const double HeadingRad = FMath::DegreesToRadians(Leader->Yaw);
		const double CosH = FMath::Cos(HeadingRad);
		const double SinH = FMath::Sin(HeadingRad);

		// Body: X=forward(North when heading=0), Y=right(East when heading=0)
		const double OffsetNorthM = Spec->FormationOffsetM.X * CosH - Spec->FormationOffsetM.Y * SinH;
		const double OffsetEastM  = Spec->FormationOffsetM.X * SinH + Spec->FormationOffsetM.Y * CosH;
		const double OffsetAltM   = -Spec->FormationOffsetM.Z; // Z=down in body frame, alt is up

		const double CosLat = FMath::Max(0.001, FMath::Abs(FMath::Cos(FMath::DegreesToRadians(Leader->Latitude))));
		Follower.Latitude  = Leader->Latitude  + OffsetNorthM / METRES_PER_DEGREE_LAT;
		Follower.Longitude = Leader->Longitude + OffsetEastM  / (METRES_PER_DEGREE_LAT * CosLat);
		Follower.Altitude  = Leader->Altitude  + static_cast<float>(OffsetAltM);

		if (Spec->bInheritHeading)
		{
			Follower.Yaw = Leader->Yaw;
		}

		// Update cache with post-formation position
		LastKnownStates.Add(Follower.EntityId, Follower);
	}
}

void FScenarioEngine::SortByFormationDependency()
{
	// Stable topological sort: entities with no LeaderEntityId first,
	// then entities whose leader appeared earlier. Handles one level of nesting.
	TArray<FCamSimConfig::FScenarioEntityConfig> Leaders;
	TArray<FCamSimConfig::FScenarioEntityConfig> Followers;

	for (const FCamSimConfig::FScenarioEntityConfig& Spec : EntityConfigs)
	{
		if (Spec.LeaderEntityId == 0)
			Leaders.Add(Spec);
		else
			Followers.Add(Spec);
	}

	EntityConfigs.Empty(Leaders.Num() + Followers.Num());
	EntityConfigs.Append(Leaders);
	EntityConfigs.Append(Followers);
}
