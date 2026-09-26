// Copyright CamSim Contributors. All Rights Reserved.

#include "Camera/CamSimPlatformRig.h"
#include "Camera/CamSimTelemetryAssembler.h"
#include "CamSimTest.h"
#include "CIGI/CigiReceiver.h"
#include "Config/CamSimConfig.h"
#include "Entity/CamSimEntity.h"
#include "Entity/CamSimEntityManager.h"
#include "Subsystem/CamSimSubsystem.h"
#include "CesiumGlobeAnchorComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

void FCamSimPlatformRig::Initialize(AActor* InOwner, UCesiumGlobeAnchorComponent* InAnchor,
	UCamSimSubsystem* InSubsystem, FCamSimTelemetryAssembler* InTelemetry, const FCamSimConfig& Cfg)
{
	Owner     = InOwner;
	Anchor    = InAnchor;
	Subsystem = InSubsystem;
	Telemetry = InTelemetry;

	if (Anchor)
	{
		Anchor->MoveToLongitudeLatitudeHeight(FVector(Cfg.StartLongitude, Cfg.StartLatitude, Cfg.StartAltitude));
		Owner->SetActorScale3D(FVector::OneVector);
		Anchor->SetEastSouthUpRotation(CamSimFrames::CigiToEastSouthUp(Cfg.StartYaw, Cfg.StartPitch, Cfg.StartRoll));
		Telemetry->SetPlatformPose(Cfg.StartLatitude, Cfg.StartLongitude, Cfg.StartAltitude,
			Cfg.StartYaw, Cfg.StartPitch, Cfg.StartRoll);

		UE_LOG(LogCamSim, Log,
			TEXT("ACamSimCamera: start position lat=%.4f lon=%.4f alt=%.0fm yaw=%.0f pitch=%.0f"),
			Cfg.StartLatitude, Cfg.StartLongitude, Cfg.StartAltitude, Cfg.StartYaw, Cfg.StartPitch);
	}

	FpsEntityId   = static_cast<uint16>(FMath::Clamp(Cfg.FpsEntityId, 0, 65535));
	FpsEyeHeightM = Cfg.FpsEyeHeightM;
	if (FpsEntityId != 0)
	{
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: FPS mode -> entity %u (config)"), FpsEntityId);
	}
}

void FCamSimPlatformRig::ApplyHostPlatformState()
{
	if (HostStateFrame == GFrameCounter) return;
	HostStateFrame = GFrameCounter;

	FCigiReceiver* Receiver = Subsystem ? Subsystem->GetCigiReceiver() : nullptr;
	if (!Receiver || !Anchor) return;

	FCigiEntityState State;
	bool bGotState = false;
	while (Receiver->DequeueCameraEntityState(State))
	{
		bGotState = true;
	}
	if (!bGotState) return;

	if (FMath::IsNaN(State.Latitude) || FMath::IsNaN(State.Longitude) || FMath::IsNaN(State.Altitude) ||
		FMath::IsNaN(State.Yaw) || FMath::IsNaN(State.Pitch) || FMath::IsNaN(State.Roll))
	{
		UE_LOG(LogCamSim, Warning, TEXT("ACamSimCamera: CIGI entity state contains NaN - skipping"));
		return;
	}

	// ~1 Hz at 30 fps so the trace stays readable when Verbose is on.
	if ((GFrameCounter % 30) == 0)
	{
		UE_LOG(LogCamSim, Verbose,
			TEXT("ACamSimCamera: CIGI -> lat=%.6f lon=%.6f alt=%.1f yaw=%.1f pitch=%.1f roll=%.1f"),
			State.Latitude, State.Longitude, State.Altitude, State.Yaw, State.Pitch, State.Roll);
	}

	bAttached = State.bAttached;
	if (bAttached)
	{
		// Attached to a platform entity: Lat/Lon/Alt are X/Y/Z offsets in the
		// parent's body frame. Resolved by FollowAttachParent().
		AttachParentId  = State.ParentId;
		AttachOffsetFrd = FVector(State.Latitude, State.Longitude, State.Altitude);
		AttachRotation  = FRotator(State.Pitch, State.Yaw, State.Roll);
	}
	else
	{
		CamSimFrames::FGeoPose Pose;
		Pose.Lat = State.Latitude;
		Pose.Lon = State.Longitude;
		Pose.Alt = State.Altitude;
		Pose.Neu = CamSimFrames::CigiToNeu(State.Yaw, State.Pitch, State.Roll);
		// Ground speed uses the *host* time of the update.
		ApplyPose(Pose, State.HostTimeSec);
	}
}

void FCamSimPlatformRig::FollowAttachParent()
{
	if (AttachFollowFrame == GFrameCounter) return;
	AttachFollowFrame = GFrameCounter;

	// Every frame, including frames without a host update (the parent may be
	// dead-reckoning).
	if (!bAttached || !Anchor || !Subsystem) return;
	CamSimFrames::FGeoPose ParentPose;
	if (AttachParentId != static_cast<uint16>(Subsystem->GetConfig().CameraEntityId)
		&& Subsystem->GetEntityGeoPose(FEntityKey(EHostSource::Cigi, AttachParentId), ParentPose))
	{
		ApplyPose(CamSimFrames::AttachedChildPose(ParentPose, AttachOffsetFrd,
			AttachRotation.Yaw, AttachRotation.Pitch, AttachRotation.Roll),
			Owner->GetWorld()->GetTimeSeconds());
	}
}

void FCamSimPlatformRig::UpdateFirstPersonView(uint16 ViewControlEntityId)
{
	if (ViewControlEntityId != 0 && ViewControlEntityId != FpsEntityId)
	{
		FpsEntityId  = ViewControlEntityId;
		bFpsFromCigi = true;
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: FPS mode -> entity %u (CIGI ViewControl)"), FpsEntityId);
	}
	else if (ViewControlEntityId == 0 && bFpsFromCigi && FpsEntityId != 0)
	{
		UE_LOG(LogCamSim, Log, TEXT("ACamSimCamera: FPS mode deactivated (CIGI ViewControl EntityId=0)"));
		FpsEntityId  = 0;
		bFpsFromCigi = false;
	}
	if (FpsEntityId == 0 || !Subsystem || !Anchor) return;

	FCamSimEntityManager* Mgr = Subsystem->GetEntityManager();
	ACamSimEntity* Entity = Mgr ? Mgr->FindEntity(FEntityKey(EHostSource::Cigi, FpsEntityId)) : nullptr;
	if (!IsValid(Entity)) return;  // not spawned yet: hold the current position
	UCesiumGlobeAnchorComponent* EntityAnchor = Entity->FindComponentByClass<UCesiumGlobeAnchorComponent>();
	if (!EntityAnchor) return;

	// Cesium LLH order: Lon, Lat, Height. Eye height is a vertical offset.
	const FVector Llh = EntityAnchor->GetLongitudeLatitudeHeight();
	const double EyeAlt = Llh.Z + static_cast<double>(FpsEyeHeightM);
	Anchor->MoveToLongitudeLatitudeHeight(FVector(Llh.X, Llh.Y, EyeAlt));
	Owner->SetActorScale3D(FVector::OneVector);

	// Inherit the entity's heading; the gimbal adds free look on top.
	const FRotator EntityHpr = CamSimFrames::EastSouthUpToCigi(EntityAnchor->GetEastSouthUpRotation());
	Anchor->SetEastSouthUpRotation(CamSimFrames::CigiToEastSouthUp(EntityHpr.Yaw, 0.0, 0.0));
	Telemetry->SetPlatformPose(Llh.Y, Llh.X, EyeAlt, EntityHpr.Yaw, 0.0f, 0.0f);
}

bool FCamSimPlatformRig::GetPose(CamSimFrames::FGeoPose& OutPose) const
{
	if (!Anchor) return false;
	const FVector Llh = Anchor->GetLongitudeLatitudeHeight();
	OutPose.Lon = Llh.X;
	OutPose.Lat = Llh.Y;
	OutPose.Alt = Llh.Z;
	OutPose.Neu = CamSimFrames::EastSouthUpToNeu(Anchor->GetEastSouthUpRotation());
	return true;
}

void FCamSimPlatformRig::ApplyPose(const CamSimFrames::FGeoPose& Pose, double TimeSec)
{
	Anchor->MoveToLongitudeLatitudeHeight(FVector(Pose.Lon, Pose.Lat, Pose.Alt));
	// Cesium's ENU transform can emit a NaN world scale during origin rebasing;
	// reset to (1,1,1) to prevent the UE SetRelativeScale3D warning.
	Owner->SetActorScale3D(FVector::OneVector);
	Anchor->SetEastSouthUpRotation(CamSimFrames::NeuToEastSouthUp(Pose.Neu));

	const FRotator Hpr = Pose.Neu.Rotator();
	Telemetry->SetPlatformPose(Pose.Lat, Pose.Lon, Pose.Alt, FRotator::ClampAxis(Hpr.Yaw), Hpr.Pitch, Hpr.Roll);

	// Ground speed (Tag 56); the estimate is held between fixes.
	GroundSpeed.AddFix(Pose.Lat, Pose.Lon, TimeSec);
	Telemetry->SetGroundSpeed(GroundSpeed.GetSpeedMps());
}
